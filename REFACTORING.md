[x] Make wrapper on RmlUI bindings using modern C++ features, so bindings should become much easier to use. Use it in whole project.
[x] Analysis of our DX12 implementation and backend implementation + bug fixes. Currently we have a bug that when we are moving application our Stream playback freezes. Maybe we need to do separate UI and logic threads. Figure it out and fix and remove workaround that fixing FPS lags while moving application window.
[x] UDP unbound information on server that will return count of users connected (not in QUIC, but like in game servers for server list)
[ ] Make complete project and network protocol analysis and find places to improve, also autoreconnect on connection drop should be implemented with reconnect to server and last channel.
[ ] Add support of multiple videos streams playback
[ ] Make support of sending and playing multiple VOIP streams (for plugins in future)
[ ] Implement plugins SDK (as dynamic libraries). As initial functionality plugins should have a possibility to create custom page in "Plugins". Plugins page should also be implemented in Parties client UI. Example Karaoke plugin should provide possibility to select .WAV file to stream it as extra audio track to server and broadcast to channel members. 
[ ] I don't like our UI management and user config store. I think we need to do UI refactor as well and improve it to be Enterprise grade
[ ] Fix issues in macOS stream encoding. Streams from macOS with M2 CPU looks very bad and like a lowbitrate stream with actual 20mb/s bitrate.
[ ] Overhaul Metal renderer and implement Slug font rasterizer there as well. Get rid of any font texture copying and implement font rendering using Slug in the best possible way
[ ] Fix bug when stream ends and user was watching stream in fullscreen mode - Parties window still stays in fullscreen