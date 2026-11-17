local setting = SettingsManager:DeclareBool('SameName', false, {})
setting.value = true
SettingsFixtureA = setting.value
Hooks:Install('SettingsScopeProbe', 0, function()
  HookFixtureA = SettingsManager:GetSetting('SameName').value
  HookRequireA = require('Probe')
end)
Hooks:Install('SettingsScopeFailure', 0, function()
  error('EXPECTED_HOOK_FAILURE')
end)
