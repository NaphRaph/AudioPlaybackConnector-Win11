#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
winrt::fire_and_forget ConnectDevice(std::wstring);
winrt::fire_and_forget ConnectDevice(DeviceInformation);
void RefreshDevicePicker();
void DisconnectDevice(std::wstring_view);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();

void QueueDeviceListRefresh()
{
	if (!g_shuttingDown && IsWindow(g_hWnd))
		PostMessageW(g_hWnd, WM_DEVICE_LIST_CHANGED, 0, 0);
}

bool IsLightTheme()
{
	DWORD value = 1;
	DWORD cbValue = sizeof(value);
	RegGetValueW(HKEY_CURRENT_USER,
		LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
		L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue);
	return value != 0;
}

winrt::Windows::UI::Color MakeColor(uint8_t alpha, uint8_t red, uint8_t green, uint8_t blue)
{
	return { alpha, red, green, blue };
}

winrt::Windows::UI::Xaml::Media::Brush CreateWin11SurfaceBrush(bool lightTheme)
{
	using namespace winrt::Windows::UI::Xaml::Media;

	AcrylicBrush brush;
	brush.BackgroundSource(AcrylicBackgroundSource::HostBackdrop);
	brush.TintColor(lightTheme ? MakeColor(255, 250, 250, 250) : MakeColor(255, 32, 32, 32));
	brush.TintOpacity(lightTheme ? 0.92 : 0.86);
	brush.FallbackColor(lightTheme ? MakeColor(255, 250, 250, 250) : MakeColor(255, 32, 32, 32));
	return brush;
}

winrt::Windows::UI::Xaml::Media::Brush CreateTextBrush(bool lightTheme, uint8_t alpha = 255)
{
	using namespace winrt::Windows::UI::Xaml::Media;
	return SolidColorBrush(lightTheme ? MakeColor(alpha, 32, 32, 32) : MakeColor(alpha, 255, 255, 255));
}

void ApplyWin11MenuStyle(MenuFlyout& menu, bool lightTheme)
{
	using namespace winrt::Windows::UI::Xaml;
	using namespace winrt::Windows::UI::Xaml::Controls;

	Style presenterStyle;
	presenterStyle.TargetType(winrt::xaml_typename<MenuFlyoutPresenter>());
	presenterStyle.Setters().Append(Setter(Control::BackgroundProperty(),
		winrt::box_value(CreateWin11SurfaceBrush(lightTheme))));
	presenterStyle.Setters().Append(Setter(Control::PaddingProperty(),
		winrt::box_value(Thickness{ 4, 6, 4, 6 })));
	presenterStyle.Setters().Append(Setter(Control::CornerRadiusProperty(),
		winrt::box_value(CornerRadius{ 10, 10, 10, 10 })));
	menu.MenuFlyoutPresenterStyle(presenterStyle);
}

bool IsCurrentConnection(std::wstring_view deviceId, uint64_t generation)
{
	std::lock_guard lock(g_connectionMutex);
	auto it = g_audioPlaybackConnections.find(std::wstring(deviceId));
	return it != g_audioPlaybackConnections.end() && it->second.Generation == generation;
}

void QueueConnectionStateChanged(std::wstring deviceId, uint64_t generation)
{
	// This callback may run on a non-UI thread. Only post a message here;
	// the connection table is intentionally touched by the UI thread alone.
	if (g_shuttingDown || !IsWindow(g_hWnd))
		return;

	auto message = std::make_unique<ConnectionStateChangedMessage>();
	message->deviceId = std::move(deviceId);
	message->generation = generation;
	if (PostMessageW(g_hWnd, WM_CONNECTION_STATE_CHANGED, 0, reinterpret_cast<LPARAM>(message.get())))
		message.release();
}

void CloseCurrentConnection(std::wstring_view deviceId, uint64_t generation)
{
	std::lock_guard lock(g_connectionMutex);
	auto it = g_audioPlaybackConnections.find(std::wstring(deviceId));
	if (it == g_audioPlaybackConnections.end() || it->second.Generation != generation)
		return;

	it->second.Connection.Close();
	g_audioPlaybackConnections.erase(it);
	g_lastConnectionCloseTimes.insert_or_assign(std::wstring(deviceId), std::chrono::steady_clock::now());
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	winrt::init_apartment();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_CLOSE:
	{
		if (g_shuttingDown.exchange(true))
			break;

		if (g_deviceWatcher)
		{
			try
			{
				g_deviceWatcher.Stop();
			}
			catch (...)
			{
				LOG_CAUGHT_EXCEPTION();
			}
			g_deviceWatcher = nullptr;
		}

		// Preserve the connected device IDs before releasing the connections only
		// when automatic reconnect was requested. Otherwise save an empty list.
		if (g_reconnect)
			SaveSettings();

		bool releasedConnections = false;
		{
			std::lock_guard lock(g_connectionMutex);
			releasedConnections = !g_audioPlaybackConnections.empty();
			for (const auto& connection : g_audioPlaybackConnections)
				connection.second.Connection.Close();
			g_audioPlaybackConnections.clear();
			g_deviceErrorMessages.clear();
		}

		if (!g_reconnect)
			SaveSettings();

		// AudioPlaybackConnection releases the underlying Bluetooth transport
		// asynchronously. Keep the process alive briefly after dropping the final
		// reference so Windows can finish deactivating the A2DP sink cleanly.
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		ShowWindow(hWnd, SW_HIDE);
		if (releasedConnections && SetTimer(hWnd, SHUTDOWN_TIMER_ID, SHUTDOWN_RELEASE_DELAY_MS, nullptr))
			break;

		DestroyWindow(hWnd);
	}
	break;
	case WM_TIMER:
		if (wParam == SHUTDOWN_TIMER_ID)
		{
			KillTimer(hWnd, SHUTDOWN_TIMER_ID);
			DestroyWindow(hWnd);
		}
		break;
	case WM_DESTROY:
		g_shuttingDown = true;
		{
			std::lock_guard lock(g_connectionMutex);
			for (const auto& connection : g_audioPlaybackConnections)
				connection.second.Connection.Close();
			g_audioPlaybackConnections.clear();
			g_deviceErrorMessages.clear();
		}
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
			SetupFlyout();
			SetupMenu();
			SetupDevicePicker();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);
			g_xamlCanvas.Width(rect.Width);
			g_xamlCanvas.Height(rect.Height);
			g_devicePickerVisible = true;
			RefreshDevicePicker();
			g_xamlDeviceFlyout.ShowAt(g_xamlCanvas);
		}
		break;
		case WM_RBUTTONUP: // Menu activated by mouse click
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
		break;
	case WM_CONNECTION_STATE_CHANGED:
	{
		std::unique_ptr<ConnectionStateChangedMessage> stateChanged(reinterpret_cast<ConnectionStateChangedMessage*>(lParam));
		if (!stateChanged || g_shuttingDown)
			break;

		{
			std::lock_guard lock(g_connectionMutex);
			auto it = g_audioPlaybackConnections.find(stateChanged->deviceId);
			if (it == g_audioPlaybackConnections.end() ||
				it->second.Generation != stateChanged->generation ||
				it->second.Connection.State() != AudioPlaybackConnectionState::Closed)
				break;
			g_audioPlaybackConnections.erase(it);
			g_lastConnectionCloseTimes.insert_or_assign(stateChanged->deviceId, std::chrono::steady_clock::now());
			QueueDeviceListRefresh();
		}
	}
	break;
	case WM_DEVICE_LIST_CHANGED:
		RefreshDevicePicker();
		break;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			for (const auto& i : g_lastDevices)
			{
				ConnectDevice(i);
			}
			g_lastDevices.clear();
		}
		break;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	using namespace winrt::Windows::UI::Xaml::Media;

	const bool lightTheme = IsLightTheme();
	const auto textBrush = CreateTextBrush(lightTheme);
	const auto secondaryTextBrush = CreateTextBrush(lightTheme, 190);

	FontIcon exitIcon;
	exitIcon.Glyph(L"\xE8BB");
	exitIcon.FontSize(22);
	exitIcon.Foreground(SolidColorBrush(MakeColor(255, 0, 103, 192)));
	exitIcon.Margin({ 0, 0, 12, 0 });

	TextBlock title;
	title.Text(_(L"Exit"));
	title.FontSize(20);
	title.FontWeight({ 600 });
	title.Foreground(textBrush);

	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.FontSize(14);
	textBlock.TextWrapping(TextWrapping::Wrap);
	textBlock.Foreground(secondaryTextBrush);
	textBlock.Margin({ 0, 8, 0, 18 });

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));
	checkbox.FontSize(14);
	checkbox.Foreground(textBrush);
	checkbox.Margin({ 0, 0, 0, 18 });

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.FontSize(14);
	button.FontWeight({ 600 });
	button.Padding({ 18, 8, 18, 8 });
	button.CornerRadius({ 6, 6, 6, 6 });
	button.Background(SolidColorBrush(MakeColor(255, 0, 103, 192)));
	button.Foreground(SolidColorBrush(MakeColor(255, 255, 255, 255)));
	button.Click([](const auto&, const auto&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel titlePanel;
	titlePanel.Orientation(Orientation::Horizontal);
	titlePanel.VerticalAlignment(VerticalAlignment::Center);
	titlePanel.Children().Append(exitIcon);
	titlePanel.Children().Append(title);

	StackPanel stackPanel;
	stackPanel.Width(320);
	stackPanel.Spacing(0);
	stackPanel.Padding({ 20, 18, 20, 18 });
	stackPanel.Children().Append(titlePanel);
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(checkbox);
	stackPanel.Children().Append(button);

	Border card;
	card.Background(CreateWin11SurfaceBrush(lightTheme));
	card.CornerRadius({ 12, 12, 12, 12 });
	card.BorderBrush(SolidColorBrush(lightTheme ? MakeColor(90, 255, 255, 255) : MakeColor(90, 255, 255, 255)));
	card.BorderThickness({ 1, 1, 1, 1 });
	card.Shadow(ThemeShadow());
	card.Child(stackPanel);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(card);

	g_xamlFlyout = flyout;
}

void SetupMenu()
{
	const bool lightTheme = IsLightTheme();

	// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		bool hasConnections = false;
		{
			std::lock_guard lock(g_connectionMutex);
			hasConnections = !g_audioPlaybackConnections.empty();
		}
		if (!hasConnections)
		{
			PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
			return;
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	MenuFlyout menu;
	menu.Items().A×_x¶‰žËkºwµçTì(%ÍÑèéÝÍÑÉ¥¹œ•ÉÉ½É5•ÍÍ…”ì(%Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¸½¹¹•Ñ¥½¸€ô¹Õ±±ÁÑÈì(%Õ¥¹ÐØÑ}Ð•¹•É…Ñ¥½¸€ô€Àì((%ÑÉä(%ì($%½¹¹•Ñ¥½¸€ôÕ‘¥½A±…å‰…­½¹¹•Ñ¥½¸èéQÉåÉ•…Ñ•É½µ%¡‘•Ù¥”¹% ¤¤ì($%¥˜€ …½¹¹•Ñ¥½¸¤($%ì($$%•ÉÉ½É5•ÍÍ…”€ô|¡0‰U¹­¹½Ý¸•ÉÉ½Èˆ¤ì($%ô($%•±Í”($%ì($$%•¹•É…Ñ¥½¸€ô€¬­}¹•áÑ½¹¹•Ñ¥½¹•¹•É…Ñ¥½¸ì($$%ì($$$%ÍÑèé±½­}Õ…É±½¬¡}½¹¹•Ñ¥½¹5ÕÑ•à¤ì($$$%}…Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹Ì¹¥¹Í•ÉÑ}½É}…ÍÍ¥¸¡‘•Ù¥•%°Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹¹ÑÉåì($$$$%½¹¹•Ñ¥½¸°•¹•É…Ñ¥½¸°ÑÉÕ”($$$%ô¤ì($$%ô($$%EÕ•Õ••Ù¥•1¥ÍÑI•™É•Í  ¤ì(($$%½¹¹•Ñ¥½¸¹MÑ…Ñ•¡…¹•¡m‘•Ù¥•%°•¹•É…Ñ¥½¹t¡½¹ÍÐ…ÕÑ¼˜Í•¹‘•È°½¹ÍÐ…ÕÑ¼˜¤ì($$$%¥˜€¡Í•¹‘•È¹MÑ…Ñ” ¤€ôôÕ‘¥½A±…å‰…­½¹¹•Ñ¥½¹MÑ…Ñ”èé±½Í•¤($$$$%EÕ•Õ•½¹¹•Ñ¥½¹MÑ…Ñ•¡…¹•¡‘•Ù¥•%°•¹•É…Ñ¥½¸¤ì($$%ô¤ì(($$$¼¼MÑ…ÉÑÍå¹Œ¥ÌÑ¥•Ñ¼Ñ¡¥Ì½¹¹•Ñ¥½¸¥¹ÍÑ…¹”¸Ù•Éä¹•Ý±äÉ•…Ñ•($$$¼¼½¹¹•Ñ¥½¸µÕÍÐ‰”•¹…‰±•‰•™½É”¥Ð¥Ì½Á•¹•¸($$%½}…Ý…¥Ð½¹¹•Ñ¥½¸¹MÑ…ÉÑÍå¹Œ ¤ì($$%½}…Ý…¥ÐÕ¥½¹Ñ•áÐì($$%¥˜€¡}Í¡ÕÑÑ¥¹½Ý¸¤($$$%½}É•ÑÕÉ¸ì($$%…ÕÑ¼É•ÍÕ±Ð€ô½}…Ý…¥Ð½¹¹•Ñ¥½¸¹=Á•¹Íå¹Œ ¤ì($$%½}…Ý…¥ÐÕ¥½¹Ñ•áÐì($$%¥˜€¡}Í¡ÕÑÑ¥¹½Ý¸¤($$$%½}É•ÑÕÉ¸ì(($$%ÍÝ¥Ñ €¡É•ÍÕ±Ð¹MÑ…ÑÕÌ ¤¤($$%ì($$%…Í”Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹=Á•¹I•ÍÕ±ÑMÑ…ÑÕÌèéMÕ•ÍÌè($$$%ÍÕ•ÍÌ€ô%ÍÕÉÉ•¹Ñ½¹¹•Ñ¥½¸¡‘•Ù¥•%°•¹•É…Ñ¥½¸¤€˜˜($$$$%½¹¹•Ñ¥½¸¹MÑ…Ñ” ¤€ôôÕ‘¥½A±…å‰…­½¹¹•Ñ¥½¹MÑ…Ñ”èé=Á•¹•ì($$$%‰É•…¬ì($$%…Í”Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹=Á•¹I•ÍÕ±ÑMÑ…ÑÕÌèéI•ÅÕ•ÍÑQ¥µ•‘=ÕÐè($$$%ÍÕ•ÍÌ€ô™…±Í”ì($$$%•ÉÉ½É5•ÍÍ…”€ô|¡0‰Q¡”É•ÅÕ•ÍÐÑ¥µ•½ÕÐˆ¤ì($$$%‰É•…¬ì($$%…Í”Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹=Á•¹I•ÍÕ±ÑMÑ…ÑÕÌèé•¹¥•‘	åMåÍÑ•´è($$$%ÍÕ•ÍÌ€ô™…±Í”ì($$$%•ÉÉ½É5•ÍÍ…”€ô|¡0‰Q¡”½Á•É…Ñ¥½¸Ý…Ì‘•¹¥•‰äÑ¡”ÍåÍÑ•´ˆ¤ì($$$%‰É•…¬ì($$%…Í”Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹=Á•¹I•ÍÕ±ÑMÑ…ÑÕÌèéU¹­¹½Ý¹…¥±ÕÉ”è($$$%ÍÕ•ÍÌ€ô™…±Í”ì($$$%ì($$$$%½¹ÍÐ…ÕÑ¼•áÑ•¹‘•‘ÉÉ½È€ôÉ•ÍÕ±Ð¹áÑ•¹‘•‘ÉÉ½È ¤ì($$$$%1=}!H¡•áÑ•¹‘•‘ÉÉ½È¤ì($$$$%Ý¡…É}Ð•ÉÉ½É½‘•lÄÙuíôì($$$$%ÍÝÁÉ¥¹Ñ™}Ì¡•ÉÉ½É½‘”°0ˆ€ Áà”Àá`¤ˆ°ÍÑ…Ñ¥}…ÍÐñÕ¥¹ÐÌÉ}Ðø¡•áÑ•¹‘•‘ÉÉ½È¤¤ì($$$$%•ÉÉ½É5•ÍÍ…”€ô|¡0‰U¹­¹½Ý¸•ÉÉ½Èˆ¤ì($$$$%•ÉÉ½É5•ÍÍ…”€¬ô•ÉÉ½É½‘”ì($$$%ô($$$%‰É•…¬ì($$%ô($%ô(%ô(%…Ñ €¡Ý¥¹ÉÐèé¡É•ÍÕ±Ñ}•ÉÉ½È½¹ÍÐ˜•à¤(%ì($%ÍÕ•ÍÌ€ô™…±Í”ì($%•ÉÉ½É5•ÍÍ…”¹É•Í¥é” ØÐ¤ì($%Ý¡¥±”€ Ä¤($%ì($$%…ÕÑ¼É•ÍÕ±Ð€ôÍÝÁÉ¥¹Ñ˜¡•ÉÉ½É5•ÍÍ…”¹‘…Ñ„ ¤°•ÉÉ½É5•ÍÍ…”¹Í¥é” ¤°0ˆ•Ì€ Áà”Àá`¤ˆ°•à¹µ•ÍÍ…” ¤¹}ÍÑÈ ¤°ÍÑ…Ñ¥}…ÍÐñÕ¥¹ÐÌÉ}Ðø¡•à¹½‘” ¤¤¤ì($$%¥˜€¡É•ÍÕ±Ð€ð€À¤($$$%•ÉÉ½É5•ÍÍ…”¹É•Í¥é”¡•ÉÉ½É5•ÍÍ…”¹Í¥é” ¤€¨€È¤ì($$%•±Í”($$%ì($$$%•ÉÉ½É5•ÍÍ…”¹É•Í¥é”¡É•ÍÕ±Ð¤ì($$$%‰É•…¬ì($$%ô($%ô($%1=}U!Q}aAQ%=8 ¤ì(%ô(%…Ñ € ¸¸¸¤(%ì($%ÍÕ•ÍÌ€ô™…±Í”ì($%•ÉÉ½É5•ÍÍ…”€ô|¡0‰U¹­¹½Ý¸•ÉÉ½Èˆ¤ì($%1=}U!Q}aAQ%=8 ¤ì(%ô((%¥˜€¡ÍÕ•ÍÌ€˜˜%ÍÕÉÉ•¹Ñ½¹¹•Ñ¥½¸¡‘•Ù¥•%°•¹•É…Ñ¥½¸¤¤(%ì($%‰½½°ÁÉ¥µ•½¹¹•Ñ¥½¸€ô™…±Í”ì($%ì($$%ÍÑèé±½­}Õ…É±½¬¡}½¹¹•Ñ¥½¹5ÕÑ•à¤ì($$%ÁÉ¥µ•½¹¹•Ñ¥½¸€ô}ÁÉ¥µ•‘•Ù¥•Ì¹¥¹Í•ÉÐ¡‘•Ù¥•%¤¹Í•½¹ì($%ô(($$¼¼]¥¹‘½ÝÌ…¸…­¹½Ý±•‘”Ñ¡”™¥ÉÍÐÉ@Í¥¹¬½¹¹•Ñ¥½¸…™Ñ•ÈÁÉ½•ÍÌ($$¼¼ÍÑ…ÉÑÕÀÝ¥Ñ¡½ÕÐ¹•½Ñ¥…Ñ¥¹œ„Ý½É­¥¹œ…Õ‘¥¼ÍÑÉ•…´¸±½Í”½½½±‘½Ý¸½½Á•¸($$¼¼å±”¥ÌÑ¡”É•±¥…‰±”É•½Ù•Éä‘½Õµ•¹Ñ•‰äÑ¡¥ÌÁÉ½©•Ð°Í¼Á•É™½É´¥Ð($$¼¼…ÕÑ½µ…Ñ¥…±±ä½¹”Á•È‘•Ù¥”¥¹ÍÑ•…½˜É•Á½ÉÑ¥¹œ„Í¥±•¹Ð½¹¹•Ñ¥½¸¸($%¥˜€¡ÁÉ¥µ•½¹¹•Ñ¥½¸¤($%ì($$%±½Í•ÕÉÉ•¹Ñ½¹¹•Ñ¥½¸¡‘•Ù¥•%°•¹•É…Ñ¥½¸¤ì($$$¼¼-••ÀÑ¡”•á¥ÍÑ¥¹œ€‰½¹¹•Ñ¥¹œˆÉ½ÜÙ¥Í¥‰±”‘ÕÉ¥¹œÑ¡”½½±‘½Ý¸Í¼Ñ¡”($$$¼¼ÕÍ•È…¹¹½ÐÍÑ…ÉÐ„Í•½¹½Ù•É±…ÁÁ¥¹œ½¹¹•Ñ¥½¸…ÑÑ•µÁÐ¸($$%½¹¹•Ñ•Ù¥”¡‘•Ù¥”¤ì($$%½}É•ÑÕÉ¸ì($%ô(($%ì($$%ÍÑèé±½­}Õ…É±½¬¡}½¹¹•Ñ¥½¹5ÕÑ•à¤ì($$%…ÕÑ¼¥Ð€ô}…Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹Ì¹™¥¹¡‘•Ù¥•%¤ì($$%¥˜€¡¥Ð€ôô}…Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹Ì¹•¹ ¤¤($$$%½}É•ÑÕÉ¸ì($$%¥Ð´ùÍ•½¹¹½¹¹•Ñ¥¹œ€ô™…±Í”ì($$%}‘•Ù¥•ÉÉ½É5•ÍÍ…•Ì¹•É…Í”¡‘•Ù¥•%¤ì($%ô(%ô(%•±Í”¥˜€ …}Í¡ÕÑÑ¥¹½Ý¸¤(%ì($%¥˜€¡•¹•É…Ñ¥½¸€„ô€À€˜˜%ÍÕÉÉ•¹Ñ½¹¹•Ñ¥½¸¡‘•Ù¥•%°•¹•É…Ñ¥½¸¤¤($$%±½Í•ÕÉÉ•¹Ñ½¹¹•Ñ¥½¸¡‘•Ù¥•%°•¹•É…Ñ¥½¸¤ì(($%ì($$%ÍÑèé±½­}Õ…É±½¬¡}½¹¹•Ñ¥½¹5ÕÑ•à¤ì($$%}‘•Ù¥•ÉÉ½É5•ÍÍ…•Ím‘•Ù¥•%‘t€ô•ÉÉ½É5•ÍÍ…”¹•µÁÑä ¤€ü|¡0‰U¹­¹½Ý¸•ÉÉ½Èˆ¤€è•ÉÉ½É5•ÍÍ…”ì($%ô(%ô((%EÕ•Õ••Ù¥•1¥ÍÑI•™É•Í  ¤ì)ô()Ù½¥¥Í½¹¹•Ñ•Ù¥”¡ÍÑèéÝÍÑÉ¥¹}Ù¥•Ü‘•Ù¥•%¤)ì(%ì($%ÍÑèé±½­}Õ…É±½¬¡}½¹¹•Ñ¥½¹5ÕÑ•à¤ì($%…ÕÑ¼¥Ð€ô}…Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹Ì¹™¥¹¡ÍÑèéÝÍÑÉ¥¹œ¡‘•Ù¥•%¤¤ì($%¥˜€¡¥Ð€„ô}…Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹Ì¹•¹ ¤¤($%ì($$%¥Ð´ùÍ•½¹¹½¹¹•Ñ¥½¸¹±½Í” ¤ì($$%}…Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹Ì¹•É…Í”¡¥Ð¤ì($$%}±…ÍÑ½¹¹•Ñ¥½¹±½Í•Q¥µ•Ì¹¥¹Í•ÉÑ}½É}…ÍÍ¥¸¡ÍÑèéÝÍÑÉ¥¹œ¡‘•Ù¥•%¤°ÍÑèé¡É½¹¼èéÍÑ•…‘å}±½¬èé¹½Ü ¤¤ì($%ô($%}‘•Ù¥•ÉÉ½É5•ÍÍ…•Ì¹•É…Í”¡ÍÑèéÝÍÑÉ¥¹œ¡‘•Ù¥•%¤¤ì(%ô(%EÕ•Õ••Ù¥•1¥ÍÑI•™É•Í  ¤ì)ô()Ù½¥‘‘•Ù¥•A¥­•ÉI½Ü¡ÍÑèéÝÍÑÉ¥¹œ½¹ÍÐ˜‘•Ù¥•%°ÍÑèéÝÍÑÉ¥¹œ½¹ÍÐ˜‘•Ù¥•9…µ”°‰½½°±¥¡ÑQ¡•µ”¤)ì(%ÕÍ¥¹œ¹…µ•ÍÁ…”Ý¥¹ÉÐèé]¥¹‘½ÝÌèéU$èéa…µ°èé5•‘¥„ì((%‰½½°½¹¹•Ñ¥¹œ€ô™…±Í”ì(%‰½½°½¹¹•Ñ•€ô™…±Í”ì(%ÍÑèéÝÍÑÉ¥¹œ•ÉÉ½É5•ÍÍ…”ì(%ì($%ÍÑèé±½­}Õ…É±½¬¡}½¹¹•Ñ¥½¹5ÕÑ•à¤ì($%…ÕÑ¼½¹¹•Ñ¥½¸€ô}…Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹Ì¹™¥¹¡‘•Ù¥•%¤ì($%½¹¹•Ñ¥¹œ€ô½¹¹•Ñ¥½¸€„ô}…Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹Ì¹•¹ ¤€˜˜½¹¹•Ñ¥½¸´ùÍ•½¹¹½¹¹•Ñ¥¹œì($$¼¼U$ÍÑ…ÑÕÌ¥Ì‘•É¥Ù•™É½´Ñ¡”½µÁ±•Ñ•½¹¹•Ñ¥½¸½Á•É…Ñ¥½¸¸Ù½¥($$¼¼ÅÕ•Éå¥¹œÑ¡”]¥¹IP…Õ‘¥¼½‰©•Ðµ•É•±äÑ¼É•Á…¥¹ÐÑ¡”ÕÍÑ½´Á¥­•È¸($%½¹¹•Ñ•€ô½¹¹•Ñ¥½¸€„ô}…Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¹Ì¹•¹ ¤€˜˜€…½¹¹•Ñ¥¹œì($%…ÕÑ¼•ÉÉ½È€ô}‘•Ù¥•ÉÉ½É5•ÍÍ…•Ì¹™¥¹¡‘•Ù¥•%¤ì($%¥˜€¡•ÉÉ½È€„ô}‘•Ù¥•ÉÉ½É5•ÍÍ…•Ì¹•¹ ¤¤($$%•ÉÉ½É5•ÍÍ…”€ô•ÉÉ½È´ùÍ•½¹ì(%ô((%É¥É½Üì(%É½Ü¹5¥¹!•¥¡Ð Øà¤ì(%É½Ü¹A…‘‘¥¹œ¡ì€à°€Ø°€à°€Øô¤ì(%É½Ü¹½É¹•ÉI…‘¥ÕÌ¡ì€à°€à°€à°€àô¤ì(%É½Ü¹	…­É½Õ¹¡M½±¥‘½±½É	ÉÕÍ ¡±¥¡ÑQ¡•µ”€ü5…­•½±½È ÈÐ°€À°€À°€À¤€è5…­•½±½È ÌÈ°€ÈÔÔ°€ÈÔÔ°€ÈÔÔ¤¤¤ì((%½±Õµ¹•™¥¹¥Ñ¥½¸¥½¹½±Õµ¸ì(%¥½¹½±Õµ¸¹]¥‘Ñ ¡É¥‘1•¹Ñ¡ì€ÐÐ°É¥‘U¹¥ÑQåÁ”èéA¥á•°ô¤ì(%½±Õµ¹•™¥¹¥Ñ¥½¸Ñ•áÑ½±Õµ¸ì(%Ñ•áÑ½±Õµ¸¹]¥‘Ñ ¡É¥‘1•¹Ñ¡ì€Ä°É¥‘U¹¥ÑQåÁ”èéMÑ…Èô¤ì(%½±Õµ¹•™¥¹¥Ñ¥½¸…Ñ¥½¹½±Õµ¸ì(%…Ñ¥½¹½±Õµ¸¹]¥‘Ñ ¡É¥‘1•¹Ñ¡ì€ÄÄÀ°É¥‘U¹¥ÑQåÁ”èéA¥á•°ô¤ì(%É½Ü¹½±Õµ¹•™¥¹¥Ñ¥½¹Ì ¤¹ÁÁ•¹¡¥½¹½±Õµ¸¤ì(%É½Ü¹½±Õµ¹•™¥¹¥Ñ¥½¹Ì ¤¹ÁÁ•¹¡Ñ•áÑ½±Õµ¸¤ì(%É½Ü¹½±Õµ¹•™¥¹¥Ñ¥½¹Ì ¤¹ÁÁ•¹¡…Ñ¥½¹½±Õµ¸¤ì((%½¹Ñ%½¸‘•Ù¥•%½¸ì(%‘•Ù¥•%½¸¹±åÁ ¡0‰qáÜÀÈˆ¤ì(%‘•Ù¥•%½¸¹½¹ÑM¥é” ÈÐ¤ì(%‘•Ù¥•%½¸¹½É•É½Õ¹¡M½±¥‘½±½É	ÉÕÍ ¡5…­•½±½È ÈÔÔ°€À°€ÄÀÌ°€ÄäÈ¤¤¤ì(%‘•Ù¥•%½¸¹!½É¥é½¹Ñ…±±¥¹µ•¹Ð¡!½É¥é½¹Ñ…±±¥¹µ•¹Ðèé•¹Ñ•È¤ì(%‘•Ù¥•%½¸¹Y•ÉÑ¥…±±¥¹µ•¹Ð¡Y•ÉÑ¥…±±¥¹µ•¹Ðèé•¹Ñ•È¤ì(%É¥èéM•Ñ½±Õµ¸¡‘•Ù¥•%½¸°€À¤ì(%É½Ü¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡‘•Ù¥•%½¸¤ì((%MÑ…­A…¹•°Ñ•áÑA…¹•°ì(%Ñ•áÑA…¹•°¹Y•ÉÑ¥…±±¥¹µ•¹Ð¡Y•ÉÑ¥…±±¥¹µ•¹Ðèé•¹Ñ•È¤ì(%Ñ•áÑA…¹•°¹5…É¥¸¡ì€à°€À°€à°€Àô¤ì((%Q•áÑ	±½¬¹…µ”ì(%¹…µ”¹Q•áÐ¡‘•Ù¥•9…µ”¹•µÁÑä ¤€ü|¡0‰U¹­¹½Ý¸‘•Ù¥”ˆ¤€è‘•Ù¥•9…µ”¤ì(%¹…µ”¹½¹ÑM¥é” ÄÔ¤ì(%¹…µ”¹½É•É½Õ¹¡É•…Ñ•Q•áÑ	ÉÕÍ ¡±¥¡ÑQ¡•µ”¤¤ì(%¹…µ”¹Q•áÑQÉ¥µµ¥¹œ¡Q•áÑQÉ¥µµ¥¹œèé¡…É…Ñ•É±±¥ÁÍ¥Ì¤ì((%Q•áÑ	±½¬ÍÑ…ÑÕÌì(%ÍÑ…ÑÕÌ¹½¹ÑM¥é” ÄÌ¤ì(%ÍÑ…ÑÕÌ¹½É•É½Õ¹¡É•…Ñ•Q•áÑ	ÉÕÍ ¡±¥¡ÑQ¡•µ”°€ÄÜÔ¤¤ì(%¥˜€¡½¹¹•Ñ¥¹œ¤($%ÍÑ…ÑÕÌ¹Q•áÐ¡|¡0‰½¹¹•Ñ¥¹œˆ¤¤ì(%•±Í”¥˜€¡½¹¹•Ñ•¤($%ÍÑ…ÑÕÌ¹Q•áÐ¡|¡0‰½¹¹•Ñ•ˆ¤¤ì(%•±Í”(%ì($%ÍÑ…ÑÕÌ¹Q•áÐ¡•ÉÉ½É5•ÍÍ…”¹•µÁÑä ¤€ü|¡0‰I•…‘äˆ¤€è•ÉÉ½É5•ÍÍ…”¤ì(%ô((%Ñ•áÑA…¹•°¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡¹…µ”¤ì(%Ñ•áÑA…¹•°¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡ÍÑ…ÑÕÌ¤ì(%É¥èéM•Ñ½±Õµ¸¡Ñ•áÑA…¹•°°€Ä¤ì(%É½Ü¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡Ñ•áÑA…¹•°¤ì((%	ÕÑÑ½¸…Ñ¥½¸ì(%…Ñ¥½¸¹5¥¹]¥‘Ñ  ÄÀÐ¤ì(%…Ñ¥½¸¹A…‘‘¥¹œ¡ì€ÄÈ°€Ø°€ÄÈ°€Øô¤ì(%…Ñ¥½¸¹½¹ÑM¥é” ÄÌ¤ì(%…Ñ¥½¸¹½É¹•ÉI…‘¥ÕÌ¡ì€Ø°€Ø°€Ø°€Øô¤ì(%…Ñ¥½¸¹%Í¹…‰±• …½¹¹•Ñ¥¹œ¤ì(%…Ñ¥½¸¹½¹Ñ•¹Ð¡Ý¥¹ÉÐèé‰½á}Ù…±Õ”¡½¹¹•Ñ•€ü|¡0‰¥Í½¹¹•Ðˆ¤€è½¹¹•Ñ¥¹œ€ü|¡0‰½¹¹•Ñ¥¹œˆ¤€è|¡0‰½¹¹•Ðˆ¤¤¤ì(%¥˜€¡½¹¹•Ñ•¤(%ì($%…Ñ¥½¸¹	…­É½Õ¹¡M½±¥‘½±½É	ÉÕÍ ¡5…­•½±½È ÈÔÔ°€À°€ÄÀÌ°€ÄäÈ¤¤¤ì($%…Ñ¥½¸¹½É•É½Õ¹¡M½±¥‘½±½É	ÉÕÍ ¡5…­•½±½È ÈÔÔ°€ÈÔÔ°€ÈÔÔ°€ÈÔÔ¤¤¤ì(%ô(%•±Í”(%ì($%…Ñ¥½¸¹	…­É½Õ¹¡M½±¥‘½±½É	ÉÕÍ ¡±¥¡ÑQ¡•µ”€ü5…­•½±½È ÌÐ°€À°€À°€À¤€è5…­•½±½È Ðà°€ÈÔÔ°€ÈÔÔ°€ÈÔÔ¤¤¤ì($%…Ñ¥½¸¹½É•É½Õ¹¡É•…Ñ•Q•áÑ	ÉÕÍ ¡±¥¡ÑQ¡•µ”¤¤ì(%ô(%…Ñ¥½¸¹±¥¬¡m‘•Ù¥•%°½¹¹•Ñ•‘t¡½¹ÍÐ…ÕÑ¼˜°½¹ÍÐ…ÕÑ¼˜¤ì($%¥˜€¡½¹¹•Ñ•¤($$%¥Í½¹¹•Ñ•Ù¥”¡‘•Ù¥•%¤ì($%•±Í”($$%½¹¹•Ñ•Ù¥”¡‘•Ù¥•%¤ì(%ô¤ì(%É¥èéM•Ñ½±Õµ¸¡…Ñ¥½¸°€È¤ì(%É½Ü¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡…Ñ¥½¸¤ì((%}‘•Ù¥•1¥ÍÑA…¹•°¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡É½Ü¤ì)ô()Ù½¥I•™É•Í¡•Ù¥•A¥­•È ¤)ì(%¥˜€¡}Í¡ÕÑÑ¥¹½Ý¸ñð€…}‘•Ù¥•1¥ÍÑA…¹•°¤($%É•ÑÕÉ¸ì((%ÍÑèéÙ•Ñ½ÈñÍÑèéÁ…¥ÈñÍÑèéÝÍÑÉ¥¹œ°ÍÑèéÝÍÑÉ¥¹œøø‘•Ù¥•Ìì(%ì($%ÍÑèé±½­}Õ…É±½¬¡}‘•Ù¥•1¥ÍÑ5ÕÑ•à¤ì($%‘•Ù¥•Ì¹É•Í•ÉÙ”¡}…Ù…¥±…‰±••Ù¥•9…µ•Ì¹Í¥é” ¤¤ì($%™½È€¡½¹ÍÐ…ÕÑ¼˜‘•Ù¥”€è}…Ù…¥±…‰±••Ù¥•9…µ•Ì¤($$%‘•Ù¥•Ì¹ÁÕÍ¡}‰…¬¡‘•Ù¥”¤ì(%ô((%½¹ÍÐ‰½½°±¥¡ÑQ¡•µ”€ô%Í1¥¡ÑQ¡•µ” ¤ì(%}‘•Ù¥•1¥ÍÑA…¹•°¹¡¥±‘É•¸ ¤¹±•…È ¤ì(%¥˜€¡‘•Ù¥•Ì¹•µÁÑä ¤¤(%ì($%Q•áÑ	±½¬•µÁÑäì($%•µÁÑä¹Q•áÐ¡}‘•Ù¥•¹Õµ•É…Ñ¥½¹½µÁ±•Ñ•€ü|¡0‰9¼½µÁ…Ñ¥‰±”…Õ‘¥¼‘•Ù¥•Ì™½Õ¹ˆ¤€è|¡0‰M•…É¡¥¹œ™½È	±Õ•Ñ½½Ñ …Õ‘¥¼‘•Ù¥•Ì¸¸¸ˆ¤¤ì($%•µÁÑä¹½¹ÑM¥é” ÄÐ¤ì($%•µÁÑä¹½É•É½Õ¹¡É•…Ñ•Q•áÑ	ÉÕÍ ¡±¥¡ÑQ¡•µ”°€ÄäÀ¤¤ì($%•µÁÑä¹Q•áÑ]É…ÁÁ¥¹œ¡Q•áÑ]É…ÁÁ¥¹œèé]É…À¤ì($%•µÁÑä¹5…É¥¸¡ì€à°€ÄØ°€à°€ÄØô¤ì($%}‘•Ù¥•1¥ÍÑA…¹•°¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡•µÁÑä¤ì(%ô(%•±Í”(%ì($%™½È€¡½¹ÍÐ…ÕÑ¼˜m‘•Ù¥•%°‘•Ù¥•9…µ•t€è‘•Ù¥•Ì¤($$%‘‘•Ù¥•A¥­•ÉI½Ü¡‘•Ù¥•%°‘•Ù¥•9…µ”°±¥¡ÑQ¡•µ”¤ì(%ô)ô()Ù½¥M•ÑÕÁ•Ù¥•A¥­•È ¤)ì(%ÕÍ¥¹œ¹…µ•ÍÁ…”Ý¥¹ÉÐèé]¥¹‘½ÝÌèéU$èéa…µ°èé5•‘¥„ì((%½¹ÍÐ‰½½°±¥¡ÑQ¡•µ”€ô%Í1¥¡ÑQ¡•µ” ¤ì(%½¹ÍÐ…ÕÑ¼Ý…Ñ¡•É•¹•É…Ñ¥½¸€ô€¬­}‘•Ù¥•]…Ñ¡•É•¹•É…Ñ¥½¸ì(%ÑÉä(%ì($%¥˜€¡}‘•Ù¥•]…Ñ¡•È¤($$%}‘•Ù¥•]…Ñ¡•È¹MÑ½À ¤ì(($%ì($$%ÍÑèé±½­}Õ…É±½¬¡}‘•Ù¥•1¥ÍÑ5ÕÑ•à¤ì($$%}…Ù…¥±…‰±••Ù¥•9…µ•Ì¹±•…È ¤ì($%ô($%}‘•Ù¥•¹Õµ•É…Ñ¥½¹½µÁ±•Ñ•€ô™…±Í”ì($%}‘•Ù¥•]…Ñ¡•È€ô•Ù¥•%¹™½Éµ…Ñ¥½¸èéÉ•…Ñ•]…Ñ¡•È¡Õ‘¥½A±…å‰…­½¹¹•Ñ¥½¸èé•Ñ•Ù¥•M•±•Ñ½È ¤¤ì($%}‘•Ù¥•]…Ñ¡•È¹‘‘•¡mÝ…Ñ¡•É•¹•É…Ñ¥½¹t¡½¹ÍÐ…ÕÑ¼˜°½¹ÍÐ…ÕÑ¼˜‘•Ù¥”¤ì($$%¥˜€¡}Í¡ÕÑÑ¥¹½Ý¸ñðÝ…Ñ¡•É•¹•É…Ñ¥½¸€„ô}‘•Ù¥•]…Ñ¡•É•¹•É…Ñ¥½¸¤($$$%É•ÑÕÉ¸ì($$%ì($$$%ÍÑèé±½­}Õ…É±½¬¡}‘•Ù¥•1¥ÍÑ5ÕÑ•à¤ì($$$%}…Ù…¥±…‰±••Ù¥•9…µ•Ì¹¥¹Í•ÉÑ}½É}…ÍÍ¥¸¡ÍÑèéÝÍÑÉ¥¹œ¡‘•Ù¥”¹% ¤¤°ÍÑèéÝÍÑÉ¥¹œ¡‘•Ù¥”¹9…µ” ¤¤¤ì($$%ô($$%¥˜€¡}‘•Ù¥•A¥­•ÉY¥Í¥‰±”€˜˜%Í]¥¹‘½Ü¡}¡]¹¤¤($$$%A½ÍÑ5•ÍÍ…•\¡}¡]¹°]5}Y%}1%MQ}!9°€À°€À¤ì($%ô¤ì($%}‘•Ù¥•]…Ñ¡•È¹I•µ½Ù•¡mÝ…Ñ¡•É•¹•É…Ñ¥½¹t¡½¹ÍÐ…ÕÑ¼˜°½¹ÍÐ…ÕÑ¼˜ÕÁ‘…Ñ”¤ì($$%¥˜€¡}Í¡ÕÑÑ¥¹½Ý¸ñðÝ…Ñ¡•É•¹•É…Ñ¥½¸€„ô}‘•Ù¥•]…Ñ¡•É•¹•É…Ñ¥½¸¤($$$%É•ÑÕÉ¸ì($$%ì($$$%ÍÑèé±½­}Õ…É±½¬¡}‘•Ù¥•1¥ÍÑ5ÕÑ•à¤ì($$$%}…Ù…¥±…‰±••Ù¥•9…µ•Ì¹•É…Í”¡ÍÑèéÝÍÑÉ¥¹œ¡ÕÁ‘…Ñ”¹% ¤¤¤ì($$%ô($$%¥˜€¡}‘•Ù¥•A¥­•ÉY¥Í¥‰±”€˜˜%Í]¥¹‘½Ü¡}¡]¹¤¤($$$%A½ÍÑ5•ÍÍ…•\¡}¡]¹°]5}Y%}1%MQ}!9°€À°€À¤ì($%ô¤ì($%}‘•Ù¥•]…Ñ¡•È¹UÁ‘…Ñ•¡mÝ…Ñ¡•É•¹•É…Ñ¥½¹t¡½¹ÍÐ…ÕÑ¼˜°½¹ÍÐ…ÕÑ¼˜¤ì($$%¥˜€¡}Í¡ÕÑÑ¥¹½Ý¸ñðÝ…Ñ¡•É•¹•É…Ñ¥½¸€„ô}‘•Ù¥•]…Ñ¡•É•¹•É…Ñ¥½¸¤($$$%É•ÑÕÉ¸ì($$%¥˜€¡}‘•Ù¥•A¥­•ÉY¥Í¥‰±”€˜˜%Í]¥¹‘½Ü¡}¡]¹¤¤($$$%A½ÍÑ5•ÍÍ…•\¡}¡]¹°]5}Y%}1%MQ}!9°€À°€À¤ì($%ô¤ì($%}‘•Ù¥•]…Ñ¡•È¹¹Õµ•É…Ñ¥½¹½µÁ±•Ñ•¡mÝ…Ñ¡•É•¹•É…Ñ¥½¹t¡½¹ÍÐ…ÕÑ¼˜°½¹ÍÐ…ÕÑ¼˜¤ì($$%¥˜€¡}Í¡ÕÑÑ¥¹½Ý¸ñðÝ…Ñ¡•É•¹•É…Ñ¥½¸€„ô}‘•Ù¥•]…Ñ¡•É•¹•É…Ñ¥½¸¤($$$%É•ÑÕÉ¸ì($$%}‘•Ù¥•¹Õµ•É…Ñ¥½¹½µÁ±•Ñ•€ôÑÉÕ”ì($$%¥˜€¡}‘•Ù¥•A¥­•ÉY¥Í¥‰±”€˜˜%Í]¥¹‘½Ü¡}¡]¹¤¤($$$%A½ÍÑ5•ÍÍ…•\¡}¡]¹°]5}Y%}1%MQ}!9°€À°€À¤ì($%ô¤ì($%}‘•Ù¥•]…Ñ¡•È¹MÑ…ÉÐ ¤ì(%ô(%…Ñ € ¸¸¸¤(%ì($%}‘•Ù¥•¹Õµ•É…Ñ¥½¹½µÁ±•Ñ•€ôÑÉÕ”ì($%1=}U!Q}aAQ%=8 ¤ì(%ô((%½¹ÍÐ…ÕÑ¼Ñ•áÑ	ÉÕÍ €ôÉ•…Ñ•Q•áÑ	ÉÕÍ ¡±¥¡ÑQ¡•µ”¤ì(%½¹ÍÐ…ÕÑ¼Í•½¹‘…ÉåQ•áÑ	ÉÕÍ €ôÉ•…Ñ•Q•áÑ	ÉÕÍ ¡±¥¡ÑQ¡•µ”°€ÄàÔ¤ì((%½¹Ñ%½¸‰±Õ•Ñ½½Ñ¡%½¸ì(%‰±Õ•Ñ½½Ñ¡%½¸¹±åÁ ¡0‰qáÜÀÈˆ¤ì(%‰±Õ•Ñ½½Ñ¡%½¸¹½¹ÑM¥é” ÈÐ¤ì(%‰±Õ•Ñ½½Ñ¡%½¸¹½É•É½Õ¹¡M½±¥‘½±½É	ÉÕÍ ¡5…­•½±½È ÈÔÔ°€À°€ÄÀÌ°€ÄäÈ¤¤¤ì(%‰±Õ•Ñ½½Ñ¡%½¸¹5…É¥¸¡ì€À°€À°€ÄÀ°€Àô¤ì((%Q•áÑ	±½¬Ñ¥Ñ±”ì(%Ñ¥Ñ±”¹Q•áÐ¡|¡0‰½¹¹•Ðˆ¤¤ì(%Ñ¥Ñ±”¹½¹ÑM¥é” ÈÀ¤ì(%Ñ¥Ñ±”¹½¹Ñ]•¥¡Ð¡ì€ØÀÀô¤ì(%Ñ¥Ñ±”¹½É•É½Õ¹¡Ñ•áÑ	ÉÕÍ ¤ì((%MÑ…­A…¹•°Ñ¥Ñ±•A…¹•°ì(%Ñ¥Ñ±•A…¹•°¹=É¥•¹Ñ…Ñ¥½¸¡=É¥•¹Ñ…Ñ¥½¸èé!½É¥é½¹Ñ…°¤ì(%Ñ¥Ñ±•A…¹•°¹Y•ÉÑ¥…±±¥¹µ•¹Ð¡Y•ÉÑ¥…±±¥¹µ•¹Ðèé•¹Ñ•È¤ì(%Ñ¥Ñ±•A…¹•°¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡‰±Õ•Ñ½½Ñ¡%½¸¤ì(%Ñ¥Ñ±•A…¹•°¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡Ñ¥Ñ±”¤ì((%Q•áÑ	±½¬ÍÕ‰Ñ¥Ñ±”ì(%ÍÕ‰Ñ¥Ñ±”¹Q•áÐ¡|¡0‰M•±•Ð„	±Õ•Ñ½½Ñ …Õ‘¥¼‘•Ù¥”ˆ¤¤ì(%ÍÕ‰Ñ¥Ñ±”¹½¹ÑM¥é” ÄÌ¤ì(%ÍÕ‰Ñ¥Ñ±”¹½É•É½Õ¹¡Í•½¹‘…ÉåQ•áÑ	ÉÕÍ ¤ì(%ÍÕ‰Ñ¥Ñ±”¹5…É¥¸¡ì€À°€Ð°€À°€Àô¤ì((%}‘•Ù¥•1¥ÍÑA…¹•°€ôMÑ…­A…¹•° ¤ì(%}‘•Ù¥•1¥ÍÑA…¹•°¹MÁ…¥¹œ Ø¤ì((%MÉ½±±Y¥•Ý•È‘•Ù¥•MÉ½±°ì(%‘•Ù¥•MÉ½±°¹5…á!•¥¡Ð ÐÈÀ¤ì(%‘•Ù¥•MÉ½±°¹5…É¥¸¡ì€À°€ÄØ°€À°€ÄØô¤ì(%‘•Ù¥•MÉ½±°¹Y•ÉÑ¥…±MÉ½±±	…ÉY¥Í¥‰¥±¥Ñä¡MÉ½±±	…ÉY¥Í¥‰¥±¥ÑäèéÕÑ¼¤ì(%‘•Ù¥•MÉ½±°¹½¹Ñ•¹Ð¡}‘•Ù¥•1¥ÍÑA…¹•°¤ì((%	ÕÑÑ½¸Í•ÑÑ¥¹Í	ÕÑÑ½¸ì(%Í•ÑÑ¥¹Í	ÕÑÑ½¸¹½¹Ñ•¹Ð¡Ý¥¹ÉÐèé‰½á}Ù…±Õ”¡|¡0‰	±Õ•Ñ½½Ñ M•ÑÑ¥¹Ìˆ¤¤¤ì(%Í•ÑÑ¥¹Í	ÕÑÑ½¸¹½¹ÑM¥é” ÄÌ¤ì(%Í•ÑÑ¥¹Í	ÕÑÑ½¸¹A…‘‘¥¹œ¡ì€ÄÈ°€Ü°€ÄÈ°€Üô¤ì(%Í•ÑÑ¥¹Í	ÕÑÑ½¸¹½É¹•ÉI…‘¥ÕÌ¡ì€Ø°€Ø°€Ø°€Øô¤ì(%Í•ÑÑ¥¹Í	ÕÑÑ½¸¹	…­É½Õ¹¡M½±¥‘½±½É	ÉÕÍ ¡±¥¡ÑQ¡•µ”€ü5…­•½±½È ÌÐ°€À°€À°€À¤€è5…­•½±½È Ðà°€ÈÔÔ°€ÈÔÔ°€ÈÔÔ¤¤¤ì(%Í•ÑÑ¥¹Í	ÕÑÑ½¸¹½É•É½Õ¹¡Ñ•áÑ	ÉÕÍ ¤ì(%Í•ÑÑ¥¹Í	ÕÑÑ½¸¹±¥¬¡mt¡½¹ÍÐ…ÕÑ¼˜°½¹ÍÐ…ÕÑ¼˜¤ì($%Ý¥¹ÉÐèé]¥¹‘½ÝÌèéMåÍÑ•´èé1…Õ¹¡•Èèé1…Õ¹¡UÉ¥Íå¹Œ¡UÉ¤¡0‰µÌµÍ•ÑÑ¥¹Ìé‰±Õ•Ñ½½Ñ ˆ¤¤ì(%ô¤ì((%	ÕÑÑ½¸…¹•±	ÕÑÑ½¸ì(%…¹•±	ÕÑÑ½¸¹½¹Ñ•¹Ð¡Ý¥¹ÉÐèé‰½á}Ù…±Õ”¡|¡0‰…¹•°ˆ¤¤¤ì(%…¹•±	ÕÑÑ½¸¹½¹ÑM¥é” ÄÌ¤ì(%…¹•±	ÕÑÑ½¸¹½¹Ñ]•¥¡Ð¡ì€ØÀÀô¤ì(%…¹•±	ÕÑÑ½¸¹A…‘‘¥¹œ¡ì€ÄØ°€Ü°€ÄØ°€Üô¤ì(%…¹•±	ÕÑÑ½¸¹½É¹•ÉI…‘¥ÕÌ¡ì€Ø°€Ø°€Ø°€Øô¤ì(%…¹•±	ÕÑÑ½¸¹	…­É½Õ¹¡M½±¥‘½±½É	ÉÕÍ ¡5…­•½±½È ÈÔÔ°€À°€ÄÀÌ°€ÄäÈ¤¤¤ì(%…¹•±	ÕÑÑ½¸¹½É•É½Õ¹¡M½±¥‘½±½É	ÉÕÍ ¡5…­•½±½È ÈÔÔ°€ÈÔÔ°€ÈÔÔ°€ÈÔÔ¤¤¤ì(%…¹•±	ÕÑÑ½¸¹±¥¬¡mt¡½¹ÍÐ…ÕÑ¼˜°½¹ÍÐ…ÕÑ¼˜¤ì($%}á…µ±•Ù¥•±å½ÕÐ¹!¥‘” ¤ì(%ô¤ì((%MÑ…­A…¹•°™½½Ñ•Èì(%™½½Ñ•È¹=É¥•¹Ñ…Ñ¥½¸¡=É¥•¹Ñ…Ñ¥½¸èé!½É¥é½¹Ñ…°¤ì(%™½½Ñ•È¹!½É¥é½¹Ñ…±±¥¹µ•¹Ð¡!½É¥é½¹Ñ…±±¥¹µ•¹ÐèéI¥¡Ð¤ì(%™½½Ñ•È¹MÁ…¥¹œ à¤ì(%™½½Ñ•È¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡Í•ÑÑ¥¹Í	ÕÑÑ½¸¤ì(%™½½Ñ•È¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡…¹•±	ÕÑÑ½¸¤ì((%MÑ…­A…¹•°½¹Ñ•¹Ðì(%½¹Ñ•¹Ð¹]¥‘Ñ  ÐÈÀ¤ì(%½¹Ñ•¹Ð¹A…‘‘¥¹œ¡ì€ÈÀ°€Äà°€ÈÀ°€Äàô¤ì(%½¹Ñ•¹Ð¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡Ñ¥Ñ±•A…¹•°¤ì(%½¹Ñ•¹Ð¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡ÍÕ‰Ñ¥Ñ±”¤ì(%½¹Ñ•¹Ð¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡‘•Ù¥•MÉ½±°¤ì(%½¹Ñ•¹Ð¹¡¥±‘É•¸ ¤¹ÁÁ•¹¡™½½Ñ•È¤ì((%	½É‘•È…Éì(%…É¹	…­É½Õ¹¡É•…Ñ•]¥¸ÄÅMÕÉ™…•	ÉÕÍ ¡±¥¡ÑQ¡•µ”¤¤ì(%…É¹½É¹•ÉI…‘¥ÕÌ¡ì€ÄÈ°€ÄÈ°€ÄÈ°€ÄÈô¤ì(%…É¹	½É‘•É	ÉÕÍ ¡M½±¥‘½±½É	ÉÕÍ ¡±¥¡ÑQ¡•µ”€ü5…­•½±½È äÀ°€ÈÔÔ°€ÈÔÔ°€ÈÔÔ¤€è5…­•½±½È äÀ°€ÈÔÔ°€ÈÔÔ°€ÈÔÔ¤¤¤ì(%…É¹	½É‘•ÉQ¡¥­¹•ÍÌ¡ì€Ä°€Ä°€Ä°€Äô¤ì(%…É¹M¡…‘½Ü¡Q¡•µ•M¡…‘½Ü ¤¤ì(%…É¹¡¥±¡½¹Ñ•¹Ð¤ì((%±å½ÕÐ™±å½ÕÐì(%™±å½ÕÐ¹M¡½Õ±‘½¹ÍÑÉ…¥¹Q½I½½Ñ	½Õ¹‘Ì¡™…±Í”¤ì(%™±å½ÕÐ¹A±…•µ•¹Ð¡Ý¥¹ÉÐèé]¥¹‘½ÝÌèéU$èéa…µ°èé½¹ÑÉ½±ÌèéAÉ¥µ¥Ñ¥Ù•Ìèé±å½ÕÑA±…•µ•¹Ñ5½‘”èéQ½À¤ì(%™±å½ÕÐ¹½¹Ñ•¹Ð¡…É¤ì(%™±å½ÕÐ¹±½Í•¡mt¡½¹ÍÐ…ÕÑ¼˜°½¹ÍÐ…ÕÑ¼˜¤ì($%}‘•Ù¥•A¥­•ÉY¥Í¥‰±”€ô™…±Í”ì($%M¡½Ý]¥¹‘½Ü¡}¡]¹°M]}!%¤ì(%ô¤ì((%}á…µ±•Ù¥•±å½ÕÐ€ô™±å½ÕÐì)ô()Ù½¥M•ÑÕÁMÙ%½¸ ¤)ì(%…ÕÑ¼¡I•Ì€ô¥¹‘I•Í½ÕÉ•\¡}¡%¹ÍÐ°5-%9QIM=UI\ Ä¤°0‰MYˆ¤ì(%%1}MQ}1MQ}II=I}%}9U10¡¡I•Ì¤ì((%…ÕÑ¼Í¥é”€ôM¥é•½™I•Í½ÕÉ”¡}¡%¹ÍÐ°¡I•Ì¤ì(%%1}MQ}1MQ}II=I}%¡Í¥é”€ôô€À¤ì((%…ÕÑ¼¡I•Í…Ñ„€ô1½…‘I•Í½ÕÉ”¡}¡%¹ÍÐ°¡I•Ì¤ì(%%1}MQ}1MQ}II=I}%}9U10¡¡I•Í…Ñ„¤ì((%…ÕÑ¼ÍÙ…Ñ„€ôÉ•¥¹Ñ•ÉÁÉ•Ñ}…ÍÐñ½¹ÍÐ¡…È¨ø¡1½­I•Í½ÕÉ”¡¡I•Í…Ñ„¤¤ì(%%1}MQ}%}9U11}11=¡ÍÙ…Ñ„¤ì((%½¹ÍÐÍÑèéÍÑÉ¥¹}Ù¥•ÜÍÙœ¡ÍÙ…Ñ„°Í¥é”¤ì(%½¹ÍÐ¥¹ÐÝ¥‘Ñ €ô•ÑMåÍÑ•µ5•ÑÉ¥Ì¡M5}aM5%=8¤°¡•¥¡Ð€ô•ÑMåÍÑ•µ5•ÑÉ¥Ì¡M5}eM5%=8¤ì((%}¡%½¹1¥¡Ð€ôMÙQ½¡%½¸¡ÍÙœ°Ý¥‘Ñ °¡•¥¡Ð°ì€À°€À°€À°€Äô¤ì(%}¡%½¹…É¬€ôMÙQ½¡%½¸¡ÍÙœ°Ý¥‘Ñ °¡•¥¡Ð°ì€Ä°€Ä°€Ä°€Äô¤ì)ô()Ù½¥UÁ‘…Ñ•9½Ñ¥™å%½¸ ¤)ì(%]=IÙ…±Õ”€ô€À°‰Y…±Õ”€ôÍ¥é•½˜¡Ù…±Õ”¤ì(%1=}%}]%8ÌÉ}II=H¡I••ÑY…±Õ•\¡!-e}UII9Q}UMH°1Hˆ¡M½™ÑÝ…É•q5¥É½Í½™Ñq]¥¹‘½ÝÍqÕÉÉ•¹ÑY•ÉÍ¥½¹qQ¡•µ•ÍqA•ÉÍ½¹…±¥é”¤ˆ°0‰MåÍÑ•µUÍ•Í1¥¡ÑQ¡•µ”ˆ°II}IQ}I}]=I°¹Õ±±ÁÑÈ°€™Ù…±Õ”°€™‰Y…±Õ”¤¤ì(%}¹¥¹¡%½¸€ôÙ…±Õ”€„ô€À€ü}¡%½¹1¥¡Ð€è}¡%½¹…É¬ì((%¥˜€ …M¡•±±}9½Ñ¥™å%½¹\¡9%5}5=%d°€™}¹¥¤¤(%ì($%¥˜€¡M¡•±±}9½Ñ¥™å%½¹\¡9%5}°€™}¹¥¤¤($%ì($$%%1}MQ}%}]%8ÌÉ}	==1}1M¡M¡•±±}9½Ñ¥™å%½¹\¡9%5}MQYIM%=8°€™}¹¥¤¤ì($%ô($%•±Í”($%ì($$%1=}1MQ}II=H ¤ì($%ô(%ô)ô