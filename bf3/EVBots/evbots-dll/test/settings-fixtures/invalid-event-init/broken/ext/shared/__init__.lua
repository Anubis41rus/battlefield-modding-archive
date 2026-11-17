Events:Subscribe('Fixture:Init', function() error('EXPECTED_INIT_EVENT_FAILURE') end)
Events:Dispatch('Fixture:Init')
