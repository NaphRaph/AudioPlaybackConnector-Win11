#include "pch.h"
#include "AudioPlaybackConnector.h"

// Windows 11 style flyouts, menus and device picker rows.

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

// XAML defaults to en-US, which makes DirectWrite fall back to SimSun for
// Chinese text. Match the XAML tree to the UI language so Chinese uses the
// same system font family as the rest of Windows.
void ApplyUiLanguage(winrt::Windows::UI::Xaml::FrameworkElement const& element)
{
	wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
	const auto language = MAKELCID(GetThreadUILanguage(), SORT_DEFAULT);
	if (LCIDToLocaleName(language, localeName, LOCALE_NAME_MAX_LENGTH, 0) > 0)
		element.Language(localeName);
}

// The default XAML font family has no CJK glyphs and falls back to SimSun.
// Use the font Windows itself uses for the same UI language instead.
winrt::Windows::UI::Xaml::Media::FontFamily UiFontFamily()
{
	const auto language = GetThreadUILanguage();
	if (PRIMARYLANGID(language) == LANG_CHINESE)
	{
		if (SUBLANGID(language) == SUBLANG_CHINESE_TRADITIONAL)
			return winrt::Windows::UI::Xaml::Media::FontFamily(L"Microsoft JhengHei UI");
		return winrt::Windows::UI::Xaml::Media::FontFamily(L"Microsoft YaHei UI");
	}
	return winrt::Windows::UI::Xaml::Media::FontFamily(L"Segoe UI Variable Text");
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

	// MenuFlyout is hosted in a separate popup window and does not reliably
	// inherit the theme of our custom surface. Apply it to every item so the
	// native template resolves normal, pointer-over, pressed, and disabled
	// foreground resources from the same light/dark theme.
	const auto itemTheme = lightTheme ? ElementTheme::Light : ElementTheme::Dark;
	for (const auto& item : menu.Items())
		item.as<FrameworkElement>().RequestedTheme(itemTheme);
}

void ApplyTransparentFlyoutStyle(Flyout& flyout)
{
	using namespace winrt::Windows::UI::Xaml;
	using namespace winrt::Windows::UI::Xaml::Controls;
	using namespace winrt::Windows::UI::Xaml::Media;

	Style presenterStyle;
	presenterStyle.TargetType(winrt::xaml_typename<FlyoutPresenter>());
	auto transparent = SolidColorBrush(MakeColor(0, 0, 0, 0));
	presenterStyle.Setters().Append(Setter(Control::BackgroundProperty(), winrt::box_value(transparent)));
	presenterStyle.Setters().Append(Setter(Control::BorderBrushProperty(), winrt::box_value(transparent)));
	presenterStyle.Setters().Append(Setter(Control::BorderThicknessProperty(),
		winrt::box_value(Thickness{ 0, 0, 0, 0 })));
	presenterStyle.Setters().Append(Setter(Control::PaddingProperty(),
		winrt::box_value(Thickness{ 0, 0, 0, 0 })));
	flyout.FlyoutPresenterStyle(presenterStyle);
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
	title.FontFamily(UiFontFamily());
	title.FontSize(20);
	title.FontWeight({ 600 });
	title.Foreground(textBrush);

	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.FontFamily(UiFontFamily());
	textBlock.FontSize(14);
	textBlock.TextWrapping(TextWrapping::Wrap);
	textBlock.Foreground(secondaryTextBrush);
	textBlock.Margin({ 0, 8, 0, 18 });

	static CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));
	checkbox.FontFamily(UiFontFamily());
	checkbox.FontSize(14);
	checkbox.Foreground(textBrush);
	checkbox.Margin({ 0, 0, 0, 18 });

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.FontFamily(UiFontFamily());
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
	card.RequestedTheme(lightTheme ? ElementTheme::Light : ElementTheme::Dark);
	ApplyUiLanguage(card);
	card.Background(CreateWin11SurfaceBrush(lightTheme));
	card.CornerRadius({ 12, 12, 12, 12 });
	card.BorderBrush(SolidColorBrush(lightTheme ? MakeColor(90, 255, 255, 255) : MakeColor(90, 255, 255, 255)));
	card.BorderThickness({ 1, 1, 1, 1 });
	card.Shadow(ThemeShadow());
	card.Child(stackPanel);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(card);
	ApplyTransparentFlyoutStyle(flyout);

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
	settingsItem.FontFamily(UiFontFamily());
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon logIcon;
	logIcon.Glyph(L"\xE8A5");

	MenuFlyoutItem logItem;
	logItem.Text(_(L"Open Logs Folder"));
	logItem.FontFamily(UiFontFamily());
	logItem.Icon(logIcon);
	logItem.Click([](const auto&, const auto&) {
		try
		{
			auto path = GetLogDirectory();
			ShellExecuteW(g_hWnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
		catch (...)
		{
			AppendLog(L"Failed to open logs folder");
		}
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.FontFamily(UiFontFamily());
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
	menu.Items().Append(settingsItem);
	menu.Items().Append(logItem);
	menu.Items().Append(exitItem);
	for (const auto& item : menu.Items())
		ApplyUiLanguage(item.as<FrameworkElement>());
	ApplyWin11MenuStyle(menu, lightTheme);
	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
		{
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
	});
	menu.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = menu;
}


void AddDevicePickerRow(std::wstring const& deviceId, std::wstring const& deviceName, bool lightTheme)
{
	using namespace winrt::Windows::UI::Xaml::Media;

	bool connecting = false;
	bool connected = false;
	bool repairing = false;
	bool canRecoverRadio = false;
	std::wstring errorMessage;
	{
		std::lock_guard lock(g_connectionMutex);
		auto connection = g_audioPlaybackConnections.find(deviceId);
		connecting = connection != g_audioPlaybackConnections.end() && connection->second.Connecting;
		auto operation = g_deviceOperations.find(deviceId);
		repairing = operation != g_deviceOperations.end() &&
			operation->second == DeviceOperation::Repairing;
		canRecoverRadio = g_radioRecoveryCandidates.find(deviceId) != g_radioRecoveryCandidates.end();
		connecting = connecting || repairing;
		// UI status is derived from the completed connection operation. Avoid
		// querying the WinRT audio object merely to repaint the custom picker.
		connected = connection != g_audioPlaybackConnections.end() && !connecting;
		auto error = g_deviceErrorMessages.find(deviceId);
		if (error != g_deviceErrorMessages.end())
			errorMessage = error->second;
	}

	Grid row;
	row.MinHeight(68);
	row.Padding({ 8, 6, 8, 6 });
	row.CornerRadius({ 8, 8, 8, 8 });
	row.Background(SolidColorBrush(lightTheme ? MakeColor(24, 0, 0, 0) : MakeColor(32, 255, 255, 255)));

	ColumnDefinition iconColumn;
	iconColumn.Width(GridLength{ 44, GridUnitType::Pixel });
	ColumnDefinition textColumn;
	textColumn.Width(GridLength{ 1, GridUnitType::Star });
	ColumnDefinition actionColumn;
	actionColumn.Width(GridLength{ connected ? 190.0 : canRecoverRadio ? 224.0 : 110.0, GridUnitType::Pixel });
	row.ColumnDefinitions().Append(iconColumn);
	row.ColumnDefinitions().Append(textColumn);
	row.ColumnDefinitions().Append(actionColumn);

	FontIcon deviceIcon;
	deviceIcon.Glyph(L"\xE702");
	deviceIcon.FontSize(24);
	deviceIcon.Foreground(SolidColorBrush(MakeColor(255, 0, 103, 192)));
	deviceIcon.HorizontalAlignment(HorizontalAlignment::Center);
	deviceIcon.VerticalAlignment(VerticalAlignment::Center);
	Grid::SetColumn(deviceIcon, 0);
	row.Children().Append(deviceIcon);

	StackPanel textPanel;
	textPanel.VerticalAlignment(VerticalAlignment::Center);
	textPanel.Margin({ 8, 0, 8, 0 });

	TextBlock name;
	name.Text(deviceName.empty() ? _(L"Unknown device") : deviceName);
	name.FontFamily(UiFontFamily());
	name.FontSize(15);
	name.Foreground(CreateTextBrush(lightTheme));
	name.TextTrimming(TextTrimming::CharacterEllipsis);

	TextBlock status;
	status.FontFamily(UiFontFamily());
	status.FontSize(13);
	status.Foreground(CreateTextBrush(lightTheme, 175));
	if (repairing)
		status.Text(g_radioRecoveryInProgress ? _(L"Restarting Bluetooth") : _(L"Repairing audio"));
	else if (connecting)
		status.Text(_(L"Connecting"));
	else if (connected)
		status.Text(_(L"Connected"));
	else
	{
		status.Text(errorMessage.empty() ? _(L"Ready") : errorMessage);
	}

	textPanel.Children().Append(name);
	textPanel.Children().Append(status);
	Grid::SetColumn(textPanel, 1);
	row.Children().Append(textPanel);

	Button action;
	action.MinWidth(104);
	action.Padding({ 12, 6, 12, 6 });
	action.FontFamily(UiFontFamily());
	action.FontSize(13);
	action.CornerRadius({ 6, 6, 6, 6 });
	action.IsEnabled(!connecting && !g_radioRecoveryInProgress);
	action.Content(winrt::box_value(connected ? _(L"Disconnect") :
		repairing ? _(L"Repairing audio") : connecting ? _(L"Connecting") : _(L"Connect")));
	if (connected)
	{
		action.Background(SolidColorBrush(MakeColor(255, 0, 103, 192)));
		action.Foreground(SolidColorBrush(MakeColor(255, 255, 255, 255)));
	}
	else
	{
		action.Background(SolidColorBrush(lightTheme ? MakeColor(34, 0, 0, 0) : MakeColor(48, 255, 255, 255)));
		action.Foreground(CreateTextBrush(lightTheme));
	}
	action.Click([deviceId, connected](const auto&, const auto&) {
		if (connected)
			DisconnectDevice(deviceId);
		else
			ConnectDevice(deviceId);
	});
	if (connected)
	{
		Button repair;
		repair.MinWidth(76);
		repair.Padding({ 10, 6, 10, 6 });
		repair.FontFamily(UiFontFamily());
		repair.FontSize(13);
		repair.CornerRadius({ 6, 6, 6, 6 });
		repair.IsEnabled(!repairing);
		repair.Content(winrt::box_value(repairing ? _(L"Repairing audio") : _(L"Repair audio")));
		repair.Background(SolidColorBrush(lightTheme ? MakeColor(34, 0, 0, 0) : MakeColor(48, 255, 255, 255)));
		repair.Foreground(CreateTextBrush(lightTheme));
		repair.Click([deviceId](const auto&, const auto&) {
			RepairDeviceAudio(deviceId);
		});

		StackPanel actions;
		actions.Orientation(Orientation::Horizontal);
		actions.HorizontalAlignment(HorizontalAlignment::Right);
		actions.VerticalAlignment(VerticalAlignment::Center);
		actions.Spacing(6);
		actions.Children().Append(repair);
		actions.Children().Append(action);
		Grid::SetColumn(actions, 2);
		row.Children().Append(actions);
	}
	else if (canRecoverRadio)
	{
		Button recover;
		recover.MinWidth(105);
		recover.Padding({ 10, 6, 10, 6 });
		recover.FontFamily(UiFontFamily());
		recover.FontSize(13);
		recover.CornerRadius({ 6, 6, 6, 6 });
		recover.IsEnabled(!connecting && !g_radioRecoveryInProgress);
		recover.Content(winrt::box_value(_(L"Restart Bluetooth")));
		recover.Background(SolidColorBrush(MakeColor(255, 0, 103, 192)));
		recover.Foreground(SolidColorBrush(MakeColor(255, 255, 255, 255)));
		recover.Click([deviceId](const auto&, const auto&) {
			const auto choice = MessageBoxW(g_hWnd,
				_(L"Restarting the computer's Bluetooth will temporarily disconnect every Bluetooth device, including keyboards and mice. Restart it and reconnect this phone?"),
				_(L"Restart Bluetooth"), MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING | MB_TOPMOST);
			if (choice == IDYES)
				RecoverBluetoothRadio(deviceId);
		});

		StackPanel actions;
		actions.Orientation(Orientation::Horizontal);
		actions.HorizontalAlignment(HorizontalAlignment::Right);
		actions.VerticalAlignment(VerticalAlignment::Center);
		actions.Spacing(6);
		actions.Children().Append(recover);
		actions.Children().Append(action);
		Grid::SetColumn(actions, 2);
		row.Children().Append(actions);
	}
	else
	{
		Grid::SetColumn(action, 2);
		row.Children().Append(action);
	}

	g_deviceListPanel.Children().Append(row);
}

void RefreshDevicePicker()
{
	if (g_shuttingDown || !g_deviceListPanel)
		return;

	std::vector<std::pair<std::wstring, std::wstring>> devices;
	{
		std::lock_guard lock(g_deviceListMutex);
		devices.reserve(g_availableDeviceNames.size());
		for (const auto& device : g_availableDeviceNames)
			devices.push_back(device);
	}

	const bool lightTheme = IsLightTheme();
	g_deviceListPanel.Children().Clear();
	if (devices.empty())
	{
		TextBlock empty;
		empty.Text(g_deviceEnumerationCompleted ? _(L"No compatible audio devices found") : _(L"Searching for Bluetooth audio devices..."));
		empty.FontFamily(UiFontFamily());
		empty.FontSize(14);
		empty.Foreground(CreateTextBrush(lightTheme, 190));
		empty.TextWrapping(TextWrapping::Wrap);
		empty.Margin({ 8, 16, 8, 16 });
		g_deviceListPanel.Children().Append(empty);
	}
	else
	{
		for (const auto& [deviceId, deviceName] : devices)
			AddDevicePickerRow(deviceId, deviceName, lightTheme);
	}
}

void SetupDevicePicker()
{
	using namespace winrt::Windows::UI::Xaml::Media;

	const bool lightTheme = IsLightTheme();
	const auto watcherGeneration = ++g_deviceWatcherGeneration;
	try
	{
		if (g_deviceWatcher)
			g_deviceWatcher.Stop();

		{
			std::lock_guard lock(g_deviceListMutex);
			g_availableDeviceNames.clear();
		}
		g_deviceEnumerationCompleted = false;
		g_deviceWatcher = DeviceInformation::CreateWatcher(AudioPlaybackConnection::GetDeviceSelector());
		g_deviceWatcher.Added([watcherGeneration](const auto&, const auto& device) {
			if (g_shuttingDown || watcherGeneration != g_deviceWatcherGeneration)
				return;
			{
				std::lock_guard lock(g_deviceListMutex);
				g_availableDeviceNames.insert_or_assign(std::wstring(device.Id()), std::wstring(device.Name()));
			}
			if (g_devicePickerVisible && IsWindow(g_hWnd))
				PostMessageW(g_hWnd, WM_DEVICE_LIST_CHANGED, 0, 0);
		});
		g_deviceWatcher.Removed([watcherGeneration](const auto&, const auto& update) {
			if (g_shuttingDown || watcherGeneration != g_deviceWatcherGeneration)
				return;
			{
				std::lock_guard lock(g_deviceListMutex);
				g_availableDeviceNames.erase(std::wstring(update.Id()));
			}
			if (g_devicePickerVisible && IsWindow(g_hWnd))
				PostMessageW(g_hWnd, WM_DEVICE_LIST_CHANGED, 0, 0);
		});
		g_deviceWatcher.Updated([watcherGeneration](const auto&, const auto&) {
			if (g_shuttingDown || watcherGeneration != g_deviceWatcherGeneration)
				return;
			if (g_devicePickerVisible && IsWindow(g_hWnd))
				PostMessageW(g_hWnd, WM_DEVICE_LIST_CHANGED, 0, 0);
		});
		g_deviceWatcher.EnumerationCompleted([watcherGeneration](const auto&, const auto&) {
			if (g_shuttingDown || watcherGeneration != g_deviceWatcherGeneration)
				return;
			g_deviceEnumerationCompleted = true;
			if (g_devicePickerVisible && IsWindow(g_hWnd))
				PostMessageW(g_hWnd, WM_DEVICE_LIST_CHANGED, 0, 0);
		});
		g_deviceWatcher.Start();
	}
	catch (...)
	{
		g_deviceEnumerationCompleted = true;
		LOG_CAUGHT_EXCEPTION();
	}

	const auto textBrush = CreateTextBrush(lightTheme);
	const auto secondaryTextBrush = CreateTextBrush(lightTheme, 185);

	FontIcon bluetoothIcon;
	bluetoothIcon.Glyph(L"\xE702");
	bluetoothIcon.FontSize(24);
	bluetoothIcon.Foreground(SolidColorBrush(MakeColor(255, 0, 103, 192)));
	bluetoothIcon.Margin({ 0, 0, 10, 0 });

	TextBlock title;
	title.Text(_(L"Connect"));
	title.FontFamily(UiFontFamily());
	title.FontSize(20);
	title.FontWeight({ 600 });
	title.Foreground(textBrush);

	StackPanel titlePanel;
	titlePanel.Orientation(Orientation::Horizontal);
	titlePanel.VerticalAlignment(VerticalAlignment::Center);
	titlePanel.Children().Append(bluetoothIcon);
	titlePanel.Children().Append(title);

	TextBlock subtitle;
	subtitle.Text(_(L"Select a Bluetooth audio device"));
	subtitle.FontFamily(UiFontFamily());
	subtitle.FontSize(13);
	subtitle.Foreground(secondaryTextBrush);
	subtitle.Margin({ 0, 4, 0, 0 });

	g_deviceListPanel = StackPanel();
	g_deviceListPanel.Spacing(6);

	ScrollViewer deviceScroll;
	deviceScroll.MaxHeight(420);
	deviceScroll.Margin({ 0, 16, 0, 16 });
	deviceScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
	deviceScroll.Content(g_deviceListPanel);

	Grid volumeRow;
	volumeRow.Margin({ 0, 0, 0, 14 });
	ColumnDefinition volumeLabelColumn;
	volumeLabelColumn.Width(GridLength{ 92, GridUnitType::Pixel });
	ColumnDefinition volumeSliderColumn;
	volumeSliderColumn.Width(GridLength{ 1, GridUnitType::Star });
	ColumnDefinition volumeValueColumn;
	volumeValueColumn.Width(GridLength{ 48, GridUnitType::Pixel });
	volumeRow.ColumnDefinitions().Append(volumeLabelColumn);
	volumeRow.ColumnDefinitions().Append(volumeSliderColumn);
	volumeRow.ColumnDefinitions().Append(volumeValueColumn);

	TextBlock volumeLabel;
	volumeLabel.Text(_(L"Playback volume"));
	volumeLabel.FontFamily(UiFontFamily());
	volumeLabel.VerticalAlignment(VerticalAlignment::Center);
	volumeLabel.Foreground(textBrush);
	Grid::SetColumn(volumeLabel, 0);
	volumeRow.Children().Append(volumeLabel);

	Slider volumeSlider;
	volumeSlider.Minimum(0);
	volumeSlider.Maximum(100);
	volumeSlider.StepFrequency(1);
	volumeSlider.Value(g_playbackVolume.load() * 100.0);
	volumeSlider.VerticalAlignment(VerticalAlignment::Center);
	Grid::SetColumn(volumeSlider, 1);
	volumeRow.Children().Append(volumeSlider);
	g_volumeSlider = volumeSlider;

	TextBlock volumeValue;
	volumeValue.FontFamily(UiFontFamily());
	volumeValue.Text(std::to_wstring(static_cast<int>(std::lround(g_playbackVolume.load() * 100.0))) + L"%");
	volumeValue.TextAlignment(TextAlignment::Right);
	volumeValue.VerticalAlignment(VerticalAlignment::Center);
	volumeValue.Foreground(secondaryTextBrush);
	Grid::SetColumn(volumeValue, 2);
	volumeRow.Children().Append(volumeValue);

	volumeSlider.ValueChanged([volumeValue](const auto& sender, const auto&) {
		const auto value = sender.as<Slider>().Value();
		g_playbackVolume.store(std::clamp(value / 100.0, 0.0, 1.0));
		g_playbackVolumeExplicit = true;
		g_volumeApplyPending = true;
		volumeValue.Text(std::to_wstring(static_cast<int>(std::lround(value))) + L"%");
	});

	Button settingsButton;
	settingsButton.Content(winrt::box_value(_(L"Bluetooth Settings")));
	settingsButton.FontFamily(UiFontFamily());
	settingsButton.FontSize(13);
	settingsButton.Padding({ 12, 7, 12, 7 });
	settingsButton.CornerRadius({ 6, 6, 6, 6 });
	settingsButton.Background(SolidColorBrush(lightTheme ? MakeColor(34, 0, 0, 0) : MakeColor(48, 255, 255, 255)));
	settingsButton.Foreground(textBrush);
	settingsButton.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	Button cancelButton;
	cancelButton.Content(winrt::box_value(_(L"Cancel")));
	cancelButton.FontFamily(UiFontFamily());
	cancelButton.FontSize(13);
	cancelButton.FontWeight({ 600 });
	cancelButton.Padding({ 16, 7, 16, 7 });
	cancelButton.CornerRadius({ 6, 6, 6, 6 });
	cancelButton.Background(SolidColorBrush(MakeColor(255, 0, 103, 192)));
	cancelButton.Foreground(SolidColorBrush(MakeColor(255, 255, 255, 255)));
	cancelButton.Click([](const auto&, const auto&) {
		g_xamlDeviceFlyout.Hide();
	});

	StackPanel footer;
	footer.Orientation(Orientation::Horizontal);
	footer.HorizontalAlignment(HorizontalAlignment::Right);
	footer.Spacing(8);
	footer.Children().Append(settingsButton);
	footer.Children().Append(cancelButton);

	StackPanel content;
	content.Width(420);
	content.Padding({ 20, 18, 20, 18 });
	content.Children().Append(titlePanel);
	content.Children().Append(subtitle);
	content.Children().Append(volumeRow);
	content.Children().Append(deviceScroll);
	content.Children().Append(footer);

	Border card;
	card.RequestedTheme(lightTheme ? ElementTheme::Light : ElementTheme::Dark);
	ApplyUiLanguage(card);
	card.Background(CreateWin11SurfaceBrush(lightTheme));
	card.CornerRadius({ 12, 12, 12, 12 });
	card.BorderBrush(SolidColorBrush(lightTheme ? MakeColor(90, 255, 255, 255) : MakeColor(90, 255, 255, 255)));
	card.BorderThickness({ 1, 1, 1, 1 });
	card.Shadow(ThemeShadow());
	card.Child(content);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Placement(winrt::Windows::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Top);
	flyout.Content(card);
	ApplyTransparentFlyoutStyle(flyout);
	flyout.Closed([](const auto&, const auto&) {
		g_devicePickerVisible = false;
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlDeviceFlyout = flyout;
}

