VEXT API, используемый ботами fun-bots
скан 114 .lua файлов в C:\Games\BF-Portable\Battlefield_3_Server\Instance\Admin\Mods\fun-bots\ext (с 16.10; раньше в C:\Games\Legacy\LegacyBF3\mods)

=== ВЫЗОВЫ МЕНЕДЖЕРОВ: Литерал:Метод (51) ===
  MathUtils:GetRandomInt                     11 файла(ов)  Server/AirTargets.lua
  NetEvents:Subscribe                         8 файла(ов)  Client/ClientNodeEditor.lua
  Events:Subscribe                            7 файла(ов)  Client/UIClient.lua
  MathUtils:GetRandom                         7 файла(ов)  Server/Bot/BotAiming.lua
  PlayerManager:GetPlayerById                 7 файла(ов)  Client/ClientBotManager.lua
  ChatManager:SendMessage                     6 файла(ов)  Server/BotManager.lua
  Events:Dispatch                             6 файла(ов)  Server/Bot/BotActions.lua
  EntityManager:GetIterator                   5 файла(ов)  Server/Bot/BotActions.lua
  NetEvents:SendToLocal                       5 файла(ов)  Server/Commands/Chat.lua
  PlayerManager:GetPlayers                    5 файла(ов)  Client/ClientBotManager.lua
  NetEvents:SendLocal                         4 файла(ов)  Client/ClientBotManager.lua
  PlayerManager:GetLocalPlayer                4 файла(ов)  Client/ClientBotManager.lua
  PlayerManager:GetPlayersByTeam              4 файла(ов)  Client/ClientBotManager.lua
  ResourceManager:SearchForDataContainer      4 файла(ов)  Server/BotSpawner.lua
  ChatManager:Yell                            3 файла(ов)  Server/Commands/Chat.lua
  Hooks:Install                               3 файла(ов)  Client/__init__.lua
  PlayerManager:GetPlayerByName               3 файла(ов)  Server/BotSpawner.lua
  PlayerManager:GetPlayerByOnlineId           3 файла(ов)  Server/BotManager.lua
  RaycastManager:CollisionRaycast             3 файла(ов)  Client/ClientBotManager.lua
  RaycastManager:Raycast                      3 файла(ов)  Client/ClientBotManager.lua
  ResourceManager:SearchForInstanceByGuid     3 файла(ов)  Server/BotSpawner.lua
  MathUtils:FNVHash                           2 файла(ов)  Server/GameDirector.lua
  NetEvents:BroadcastLocal                    2 файла(ов)  Server/SettingsManager.lua
  NetEvents:SendTo                            2 файла(ов)  Server/UIPathMenu.lua
  PlayerManager:DeletePlayer                  2 файла(ов)  Server/Bot/Bot.lua
  PlayerManager:GetPlayerCount                2 файла(ов)  Server/BotManager.lua
  PlayerManager:GetPlayersBySquad             2 файла(ов)  Server/BotSpawner.lua
  SharedUtils:GetCurrentGameMode              2 файла(ов)  Server/BotSpawner.lua
  SharedUtils:GetLevelName                    2 файла(ов)  Server/__init__.lua
  EntityManager:CreateEntitiesFromBlueprint   1 файла(ов)  Server/__init__.lua
  Net:GetHTTPAsync                            1 файла(ов)  Server/UpdateCheck.lua
  NetEvents:Send                              1 файла(ов)  Client/UIClient.lua
  NetEvents:SendUnreliableToLocal             1 файла(ов)  Server/BotManager.lua
  PlayerManager:CreatePlayer                  1 файла(ов)  Server/BotManager.lua
  RaycastManager:DetailedRaycast              1 файла(ов)  Server/NodeEditor.lua
  ResourceManager:AddRegistry                 1 файла(ов)  Shared/__init__.lua
  ResourceManager:FindDatabasePartition       1 файла(ов)  Shared/EbxEditUtils.lua
  ResourceManager:GetSettings                 1 файла(ов)  Server/__init__.lua
  ResourceManager:MountSuperBundle            1 файла(ов)  Shared/__init__.lua
  ResourceManager:RegisterInstanceLoadHandler  1 файла(ов)  Server/__init__.lua
  ServerUtils:GetCustomGameModeName           1 файла(ов)  Server/__init__.lua
  SharedUtils:GetTickrate                     1 файла(ов)  Server/BotManager.lua
  SharedUtils:GetTime                         1 файла(ов)  Client/__init__.lua
  SharedUtils:GetTimeNS                       1 файла(ов)  Shared/Utils/Profiler.lua
  SharedUtils:IsClientModule                  1 файла(ов)  Shared/EbxEditUtils.lua
  SharedUtils:IsServerModule                  1 файла(ов)  Shared/EbxEditUtils.lua
  TeamSquadManager:GetSquadPlayerCount        1 файла(ов)  Server/BotSpawner.lua
  TicketManager:GetCurrentRound               1 файла(ов)  Server/__init__.lua
  TicketManager:GetRoundCount                 1 файла(ов)  Server/__init__.lua
  TicketManager:GetTicketCount                1 файла(ов)  Server/BotSpawner.lua
  Vec3:Distance                               1 файла(ов)  Server/NodeCollection.lua

=== ИМЕНА В СТРОКАХ Class:Method (123) ===
  Bot:AbortWait                               3  Server/Bot/BotActions.lua
  ClientNodeEditor:ClearTrace                 3  Client/ClientNodeEditor.lua
  Extension:Loaded                            3  Client/__init__.lua
  NodeCollection:Clear                        3  Server/Commands/Chat.lua
  Bot:ExitVehicle                             2  Server/Bot/VehicleMovement.lua
  Botmanager:RaycastResults                   2  Client/ClientBotManager.lua
  Client:RequestChangeVehicleSeat             2  Client/ClientBotManager.lua
  Client:RequestEnterVehicle                  2  Client/ClientBotManager.lua
  Client:RequestSettings                      2  Client/ClientBotManager.lua
  ClientNodeEditor:AddNodes                   2  Client/ClientNodeEditor.lua
  ClientNodeEditor:ClearAll                   2  Client/ClientNodeEditor.lua
  ClientNodeEditor:ClearCustomTrace           2  Client/ClientNodeEditor.lua
  ClientNodeEditor:RegisterEvents             2  Client/__init__.lua
  ClientNodeEditor:RemoveNodes                2  Client/ClientNodeEditor.lua
  ClientNodeEditor:RevieveNodes               2  Client/ClientNodeEditor.lua
  ClientNodeEditor:SelectNewNode              2  Client/ClientNodeEditor.lua
  ClientNodeEditor:UpdateNodes                2  Client/ClientNodeEditor.lua
  ClientNodeEditor:UpdateSelection            2  Client/ClientNodeEditor.lua
  ConsoleCommands:DestroyObstaclesTest        2  Client/ConsoleCommands.lua
  ConsoleCommands:PrintResponse               2  Client/__init__.lua
  ConsoleCommands:RegisterCommands            2  Client/__init__.lua
  ConsoleCommands:Restore                     2  Client/ConsoleCommands.lua
  ConsoleCommands:SaveAll                     2  Client/ConsoleCommands.lua
  ConsoleCommands:SetConfig                   2  Client/ConsoleCommands.lua
  ConsoleCommands:SpawnGrenade                2  Client/ConsoleCommands.lua
  Engine:Update                               2  Client/__init__.lua
  Extension:Unloading                         2  Client/__init__.lua
  Level:Destroy                               2  Client/__init__.lua
  Level:Loaded                                2  Client/__init__.lua
  NodeCollection:FinishedLoading              2  Server/NodeCollection.lua
  NodeEditor:AddMcom                          2  Client/ClientNodeEditor.lua
  NodeEditor:AddNode                          2  Client/ClientNodeEditor.lua
  NodeEditor:AddObjective                     2  Client/ClientNodeEditor.lua
  NodeEditor:AddVehicle                       2  Client/ClientNodeEditor.lua
  NodeEditor:AddVehiclePath                   2  Client/ClientNodeEditor.lua
  NodeEditor:ClearSelection                   2  Client/ClientNodeEditor.lua
  NodeEditor:CustomAction                     2  Client/ClientNodeEditor.lua
  NodeEditor:Deselect                         2  Client/ClientNodeEditor.lua
  NodeEditor:ExitVehicle                      2  Client/ClientNodeEditor.lua
  NodeEditor:JumpDetected                     2  Client/ClientNodeEditor.lua
  NodeEditor:LinkNodes                        2  Client/ClientNodeEditor.lua
  NodeEditor:MergeNodes                       2  Client/ClientNodeEditor.lua
  NodeEditor:RemoveAllObjectives              2  Client/ClientNodeEditor.lua
  NodeEditor:RemoveData                       2  Client/ClientNodeEditor.lua
  NodeEditor:RemoveNode                       2  Client/ClientNodeEditor.lua
  NodeEditor:RemoveObjective                  2  Client/ClientNodeEditor.lua
  NodeEditor:RequestData                      2  Client/ClientNodeEditor.lua
  NodeEditor:Select                           2  Client/ClientNodeEditor.lua
  NodeEditor:SelectBetween                    2  Client/ClientNodeEditor.lua
  NodeEditor:SelectNext                       2  Client/ClientNodeEditor.lua
  NodeEditor:SelectPrevious                   2  Client/ClientNodeEditor.lua
  NodeEditor:SetInputNode                     2  Client/ClientNodeEditor.lua
  NodeEditor:SetVehicleSpawn                  2  Client/ClientNodeEditor.lua
  NodeEditor:SpawnBot                         2  Client/ClientNodeEditor.lua
  NodeEditor:SplitNode                        2  Client/ClientNodeEditor.lua
  NodeEditor:TeleportToEdge                   2  Client/ClientNodeEditor.lua
  NodeEditor:UnlinkNodes                      2  Client/ClientNodeEditor.lua
  NodeEditor:UpdatePos                        2  Client/ClientNodeEditor.lua
  PathMenu:Hide                               2  Client/UIClient.lua
  PathMenu:Request                            2  Client/UIClient.lua
  PathMenu:Unhide                             2  Client/UIClient.lua
  Player:Respawn                              2  Client/__init__.lua
  SpawnPointHelper:TeleportTo                 2  Client/ClientSpawnPointHelper.lua
  UpdateManager:Update                        2  Client/__init__.lua
  Bot:SoldierEntity                           1  Server/BotSpawner.lua
  BotSpawner:AfterSpawn                       1  Server/BotSpawner.lua
  BotSpawner:Spawn                            1  Server/BotSpawner.lua
  BotSpawner:SpawnPart2                       1  Server/BotSpawner.lua
  BotSpawner:SpawnPart3                       1  Server/BotSpawner.lua
  Botmanager:Update                           1  Server/BotManager.lua
  BulletEntity:Collision                      1  Server/__init__.lua
  CapturePoint:Captured                       1  Server/__init__.lua
  CapturePoint:Lost                           1  Server/__init__.lua
  Client:UpdateInput                          1  Client/__init__.lua
  ClientNodeEditor:EndTrace                   1  Server/Commands/Chat.lua
  ClientNodeEditor:PrintLog                   1  Client/ClientNodeEditor.lua
  ClientNodeEditor:SaveTrace                  1  Server/Commands/Chat.lua
  ClientNodeEditor:StartTrace                 1  Server/Commands/Chat.lua
  CombatArea:PlayerDeserting                  1  Server/__init__.lua
  CombatArea:PlayerReturning                  1  Server/__init__.lua
  Engine:Init                                 1  Server/__init__.lua
  Engine:Message                              1  Client/__init__.lua
  EntityFactory:Create                        1  Server/__init__.lua
  GameDirector:Update1                        1  Server/GameDirector.lua
  GameDirector:Update2                        1  Server/GameDirector.lua
  GameDirector:Update3                        1  Server/GameDirector.lua
  Input:PreUpdate                             1  Client/__init__.lua
  Level:LoadResources                         1  Shared/__init__.lua
  Level:RegisterEntityResources               1  Shared/__init__.lua
  LifeCounter:BaseDestroyed                   1  Server/__init__.lua
  NodeCollection:Create                       1  Server/NodeCollection.lua
  NodeEditor:AddSpawnPath                     1  Server/NodeEditor.lua
  NodeEditor:SetLoopMode                      1  Client/ClientNodeEditor.lua
  NodeEditor:SetPathLoops                     1  Server/NodeEditor.lua
  NodeEditor:SetSpawnPath                     1  Client/ClientNodeEditor.lua
  Partition:Loaded                            1  Client/__init__.lua
  PathMenu:Close                              1  Server/UIPathMenu.lua
  PathMenu:Open                               1  Server/UIPathMenu.lua
  Player:Authenticated                        1  Server/__init__.lua
  Player:Chat                                 1  Server/__init__.lua
  Player:Deleted                              1  Client/__init__.lua
  Player:Destroyed                            1  Server/__init__.lua
  Player:EnteredCapturePoint                  1  Server/__init__.lua
  Player:ExitedCapturePoint                   1  Server/__init__.lua
  Player:Joining                              1  Server/__init__.lua
  Player:Killed                               1  Server/__init__.lua
  Player:Left                                 1  Server/__init__.lua
  Player:TeamChange                           1  Server/__init__.lua
  ResourceManager:LoadBundles                 1  Shared/__init__.lua
  ScoringSystem:StatEvent                     1  Server/__init__.lua
  Server:RoundOver                            1  Server/__init__.lua
  Server:RoundReset                           1  Server/__init__.lua
  Soldier:Damage                              1  Server/__init__.lua
  Soldier:HealthAction                        1  Server/__init__.lua
  UI:DrawHud                                  1  Client/__init__.lua
  UI:PushScreen                               1  Client/__init__.lua
  UIClient:UI_Toggle                          1  Client/UIClient.lua
  Vehicle:Damage                              1  Server/__init__.lua
  Vehicle:Enter                               1  Server/__init__.lua
  Vehicle:Exit                                1  Server/__init__.lua
  Vehicle:SpawnDone                           1  Server/__init__.lua
  Vehicle:Unspawn                             1  Server/__init__.lua
  WaypointEditor:ChangeMode                   1  Client/ClientNodeEditor.lua

=== ЗАРЕГИСТРИРОВАННЫЕ ХУКИ/ИВЕНТЫ (59) ===
  BotEditor                                   2  Client/UIClient.lua
  UI_Request_Save_Settings                    2  Server/NodeEditor.lua
  UI_Settings                                 2  Client/UIClient.lua
  Add                                         1  Client/ClientNodeEditor.lua
  AddMcom                                     1  Client/ClientNodeEditor.lua
  AddObjective                                1  Client/ClientNodeEditor.lua
  AddSpawnPath                                1  Client/ClientNodeEditor.lua
  AddVehicle                                  1  Client/ClientNodeEditor.lua
  AddVehiclePath                              1  Client/ClientNodeEditor.lua
  AddVehicleSpawn                             1  Client/ClientNodeEditor.lua
  CheckBotBotAttack                           1  Client/__init__.lua
  ClearSelection                              1  Client/ClientNodeEditor.lua
  CustomAction                                1  Client/ClientNodeEditor.lua
  DestroyObstaclesTest                        1  Client/ConsoleCommands.lua
  Enabled                                     1  Client/ClientNodeEditor.lua
  ExitVehicle                                 1  Client/ClientNodeEditor.lua
  Link                                        1  Client/ClientNodeEditor.lua
  Merge                                       1  Client/ClientNodeEditor.lua
  Move                                        1  Client/ClientNodeEditor.lua
  Remove                                      1  Client/ClientNodeEditor.lua
  RemoveAllObjectives                         1  Client/ClientNodeEditor.lua
  RemoveObjective                             1  Client/ClientNodeEditor.lua
  Select                                      1  Client/ClientNodeEditor.lua
  SelectBetween                               1  Client/ClientNodeEditor.lua
  SelectNext                                  1  Client/ClientNodeEditor.lua
  SelectPrevious                              1  Client/ClientNodeEditor.lua
  SetInput                                    1  Client/ClientNodeEditor.lua
  SetPathLoops                                1  Client/ClientNodeEditor.lua
  SpawnGrenade                                1  Client/ConsoleCommands.lua
  Split                                       1  Client/ClientNodeEditor.lua
  UI_Change_Language                          1  Client/UIClient.lua
  UI_ClientNodeEditor_TraceData               1  Client/ClientNodeEditor.lua
  UI_Close                                    1  Client/UIViews.lua
  UI_CommoRose                                1  Client/UIClient.lua
  UI_CommoRose_Action_Add                     1  Client/ClientNodeEditor.lua
  UI_CommoRose_Action_ClearSelections         1  Client/ClientNodeEditor.lua
  UI_CommoRose_Action_Link                    1  Client/ClientNodeEditor.lua
  UI_CommoRose_Action_Merge                   1  Client/ClientNodeEditor.lua
  UI_CommoRose_Action_Move                    1  Client/ClientNodeEditor.lua
  UI_CommoRose_Action_Remove                  1  Client/ClientNodeEditor.lua
  UI_CommoRose_Action_Select                  1  Client/ClientNodeEditor.lua
  UI_CommoRose_Action_SelectBetween           1  Client/ClientNodeEditor.lua
  UI_CommoRose_Action_SelectNext              1  Client/ClientNodeEditor.lua
  UI_CommoRose_Action_SelectPrevious          1  Client/ClientNodeEditor.lua
  UI_CommoRose_Action_SetInput                1  Client/ClientNodeEditor.lua
  UI_CommoRose_Action_Split                   1  Client/ClientNodeEditor.lua
  UI_CommoRose_Action_Unlink                  1  Client/ClientNodeEditor.lua
  UI_Request_CommoRose_Show                   1  Server/UIServer.lua
  UI_Request_Open                             1  Server/UIServer.lua
  UI_Save_Settings                            1  Client/UIClient.lua
  UI_Show_Toolbar                             1  Client/UIClient.lua
  UI_Toggle                                   1  Client/UIClient.lua
  UI_Trace                                    1  Client/UIClient.lua
  UI_Trace_Index                              1  Client/UIClient.lua
  UI_Trace_Waypoints                          1  Client/UIClient.lua
  UI_Waypoints_Disable                        1  Client/UIClient.lua
  UI_Waypoints_Editor                         1  Client/UIClient.lua
  Unlink                                      1  Client/ClientNodeEditor.lua
  WriteClientSettings                         1  Client/__init__.lua