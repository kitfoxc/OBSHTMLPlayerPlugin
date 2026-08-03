# Native OBS Plugin to Display Now Playing Widget

<img width="1071" height="1998" alt="image" src="https://github.com/user-attachments/assets/1809def0-d924-4686-963e-f34f96e825da" />



This plugin allows you to have a native "now playing" widget inside of OBS. No need for additional programs or web servers or browser sources or any of that stuff. This is all native inside of OBS.

Works for Spotify, YoutubeMusic desktop clients, and Apple Music. More services may be added later if they are requested and I can reasonably get a copy of the program.

# How to use

Download the latest release from the releases page. Unzip, and put all 4 files in your OBS plugins directory (usually something like `C:\Program Files\OBS\obs-plugins\64bit\`). Launch OBS and add a new source, choose the new option called Now Playing Widget. Configure the settings using the built in options.

# Known limitations

Only works on windows. 

# Troubleshooting

If the plugin is not working, you are probably missing .NET Framework 4.7.2 , which you can download from microsoft here: https://dotnet.microsoft.com/en-us/download/dotnet-framework/net472

You only need the runtime, not the developer pack, but either will get you the components you need.

You will also need VC 2015-2022 runtime, which you can also download from microsoft here: https://aka.ms/vs/17/release/vc_redist.x64.exe

You may also need the VC Redist 2015, which is again hosted by microsoft and can be found here: https://www.microsoft.com/en-ie/download/details.aspx?id=48145

# Compilation from source notes:

Requires the following libraries to compile properly:

https://github.com/lingeriegoat/SpotifyReader
https://github.com/lingeriegoat/CppSpotifyReaderDLLBridge

The spotify reader DLL is written in c# because I am more comfortable working with windows using c#

Most of the cpp code is AI generated because I am awful at writing cpp. Seems to work fine though.

If you want to get the c# dll function working natively in cpp so this can be a single dll instead of 3 dlls, please feel free to fork this and let me know, id love to see that. My cpp is so bad i tried but cannot do it, even with AI help.
