VENICEEXT API — инвентарь из VU-Docs
источник: C:\Games\Legacy\VU-Docs\content\vext\ref

=== Сводка ===
side      event   hook  library     type    всего
client       30     24       14       55      123
server       51      9       10       21       91
shared       16      4        8       61       89
ВСЕГО        97     37       32      137      303

=== HOOK (перехватчики) ===
-- client (24)
   Bulletentity:collision
   Client:updateclientgameviewtransform
   Client:updatefirstpersontransform
   Clientchatmanager:incomingmessage
   Entityfactory:create
   Entityfactory:createfromblueprint
   Input:preupdate
   Ui:createaction
   Ui:createchatmessage
   Ui:createkilledothermessage
   Ui:createkillmessage
   Ui:createselfkilledmessage
   Ui:drawenemynametag
   Ui:drawfriendlynametag
   Ui:drawmorenametags
   Ui:drawnametags
   Ui:drawsquadnametag
   Ui:enablecursormode
   Ui:enabletypingmode
   Ui:inputconceptevent
   Ui:nametagsshouldupdate
   Ui:pushscreen
   Ui:renderminimap
   Ui:updatenametagicons
-- server (9)
   Bulletentity:collision
   Entityfactory:create
   Entityfactory:createfromblueprint
   Player:findbestsquad
   Player:findbestteam
   Player:requestjoin
   Player:selectteam
   Server:suppressenemies
   Soldier:damage
-- shared (4)
   Entity:shouldcollidewith
   Resourcemanager:loadbundles
   Terrain:load
   Visualterrain:load

=== EVENT (события) ===
-- client (30)
   Client:aimtargetchanged
   Client:postframeupdate
   Client:updateinput
   Fpscamera:update
   Freecam:postframeupdate
   Level:finalized
   Level:loaded
   Player:changingweapon
   Player:connected
   Player:deleted
   Player:killed
   Player:respawn
   Player:setsquadleader
   Player:squadchange
   Player:teamchange
   Player:updateinput
   Settingsmanager:settingschanged
   Soldier:healthaction
   Soldier:spawn
   Ui:drawhud
   Ve:stateadded
   Ve:stateremoved
   Ve:statescleared
   Voip:channelclosed
   Voip:channelopened
   Voipchannel:playerjoined
   Voipchannel:playerleft
   Voipemitter:emitting
   Vu:spec_postupdate
   Vu:spec_preupdate
-- server (51)
   Capturepoint:captured
   Capturepoint:lost
   Combatarea:playerdeserting
   Combatarea:playerreturning
   Engine:init
   Level:loaded
   Lifecounter:basedestroyed
   Player:authenticated
   Player:changingweapon
   Player:chat
   Player:created
   Player:destroyed
   Player:enteredcapturepoint
   Player:enteredlevel
   Player:exitedcapturepoint
   Player:instantsuicide
   Player:joining
   Player:kickedfromsquad
   Player:killed
   Player:kitpickup
   Player:left
   Player:mandownrevived
   Player:reload
   Player:respawn
   Player:resupply
   Player:reviveaccepted
   Player:reviverefused
   Player:score
   Player:setsquad
   Player:setsquadleader
   Player:spawnatvehicle
   Player:spawnonplayer
   Player:spawnonselectedspawnpoint
   Player:squadchange
   Player:suppressedenemy
   Player:teamchange
   Player:update
   Player:updateinteract
   Scoringsystem:statevent
   Server:roundover
   Server:roundreset
   Soldier:healthaction
   Soldier:mandown
   Soldier:prephysicsupdate
   Vehicle:damage
   Vehicle:destroyed
   Vehicle:disabled
   Vehicle:enter
   Vehicle:exit
   Vehicle:spawndone
   Vehicle:unspawn
-- shared (16)
   Engine:message
   Engine:update
   Extension:loaded
   Extension:unloading
   Gunsway:update
   Gunsway:updaterecoil
   Level:destroy
   Level:loadinginfo
   Level:loadresources
   Level:registerentityresources
   Partition:loaded
   Resourcemanager:clearcompartment
   Updatemanager:update
   Weaponfiring:create
   Weaponfiring:update
   Weaponfiring:updateammo

=== LIBRARY (с описанием, >=1 КБ) ===
client   Chatmanager                          1696 Б  
client   Clientutils                          3977 Б  
client   Console                              2635 Б  
client   Debugrenderer                        4987 Б  
client   Effectmanager                        4220 Б  
client   Inputmanager                         6774 Б  
client   Netevents                            3532 Б  
client   Playermanager                        4906 Б  
client   Raycastmanager                       5129 Б  
client   Settingsmanager                      5880 Б  
client   Spectatormanager                     3048 Б  
client   Visualenvironmentmanager             3802 Б  
client   Voip                                 1699 Б  
client   Webui                                2170 Б  
server   Chatmanager                          5740 Б  
server   Netevents                            6643 Б  
server   Playermanager                        7389 Б  
server   Raycastmanager                       5117 Б  
server   Rcon                                 3584 Б  
server   Serverutils                          2136 Б  
server   Sql                                  3621 Б  
server   Teamsquadmanager                     1115 Б  
server   Ticketmanager                        1418 Б  
server   Voip                                 1546 Б  
shared   Entitymanager                        5116 Б  
shared   Events                               2133 Б  
shared   Hooks                                1245 Б  
shared   Mathutils                            4282 Б  
shared   Net                                 27982 Б  
shared   Resourcemanager                     17442 Б  
shared   Sharedutils                          2784 Б  
shared   Terrainediting                       2312 Б  

=== TYPE (топ-30 по размеру) ===
shared   Messagetype                         42119 Б  
server   Player                              16017 Б  
shared   Datacontainer                       11205 Б  
shared   Uiaction                            10283 Б  
server   Physicsentity                       10001 Б  
shared   Entitybuspeer                        9876 Б  
shared   Entitybus                            8948 Б  
shared   Physicsentitybase                    8863 Б  
shared   Vec3                                 8775 Б  
client   Ragdollcomponent                     8761 Б  
shared   Component                            8689 Б  
client   Physicsentity                        8188 Б  
shared   Lineartransform                      8116 Б  
server   Soldierentity                        7933 Б  
client   Sunflaredata                         7663 Б  
client   Visualenvironmentstate               7248 Б  
shared   Vec2                                 6668 Б  
client   Soldierentity                        6618 Б  
shared   Vec4                                 6300 Б  
shared   Quat                                 6248 Б  
client   Skydata                              6210 Б  
server   Spawnentity                          5069 Б  
server   Controllableentity                   4883 Б  
client   Player                               4682 Б  
client   Gameentity                           4563 Б  
server   Gameentity                           4563 Б  
server   Capturepointentity                   4426 Б  
shared   Entity                               4269 Б  
client   Effectparams                         4129 Б  
client   Controllableentity                   3923 Б  