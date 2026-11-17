SettingsFixtureB = SettingsManager:DeclareBool('SameName', false, {}).value
Hooks:Install('SettingsScopeProbe', 0, function()
  HookFixtureB = SettingsManager:GetSetting('SameName').value
  HookRequireB = require('Probe')
end)
