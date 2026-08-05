# Native OBS Plugin to Display Now Playing Widget

<img width="1066" height="1803" alt="image" src="https://github.com/user-attachments/assets/0e35d43c-a671-4a59-b751-1b98bac01914" />

Vertical layout enable:

<img width="439" height="605" alt="image" src="https://github.com/user-attachments/assets/b3856bcb-d0b8-4714-8e75-7de41b475d8f" />



This plugin allows you to have a native "now playing" widget inside of OBS. No need for additional programs or web servers or browser sources or any of that stuff. This is all native inside of OBS.

Works for Spotify, YoutubeMusic desktop clients, and Apple Music. More services may be added later if they are requested and I can reasonably get a copy of the program.

# How to use

Download the latest release from the releases page. Unzip into the root of your OBS directory (usually something like `C:\Program Files\obs-studio\`). Launch OBS and add a new source, choose the new option called Now Playing Widget. Configure the settings using the built in options.

<img width="1037" height="414" alt="obsplugininstallinstructions" src="https://github.com/user-attachments/assets/2b5765cf-1afe-4489-970a-1a52e7b46357" />


# Known limitations

Only works on windows. 

# Troubleshooting

### The widget is blank, or its only show the name of the widget and this weird picture of a goat!

If the preview or the live source looks like this:

<img width="1044" height="280" alt="image" src="https://github.com/user-attachments/assets/87d287a3-5623-4082-89d4-2fa2e439608a" />

or like this:

<img width="1041" height="248" alt="image" src="https://github.com/user-attachments/assets/65fccbd2-6f99-4340-82e8-cb5a73c62436" />

You either do not have any songs playing, or you are not using windows 10 or 11, or you are using an incompatible source. If its the latter case, please make an issue here on git, or a post on the OBS plugin thread (https://obsproject.com/forum/threads/native-nowplaying-widget-for-obs.196014/) describing what software you are trying to use with the plugin, and where it can be downloaded, and I can look into whether its possible to add support. 

### The Artist text is very small

If you had an older version of the plugin, the font has probably defaulted now that it can be changed separately. Simply open the options and select a new font. The old default was -2px smaller than whatever the Title font is, and "Regular" weight (ie, not bold or italic).

### The plugin is not loading at all!

If the plugin doesnt load at all, you are probably missing .NET Framework 4.7.2 , which you can download from microsoft here: https://dotnet.microsoft.com/en-us/download/dotnet-framework/net472

You only need the runtime, not the developer pack, but either will get you the components you need.

You will also need VC 2015-2022 runtime, which you can also download from microsoft here: https://aka.ms/vs/17/release/vc_redist.x64.exe

You may also need the VC Redist 2015, which is again hosted by microsoft and can be found here: https://www.microsoft.com/en-ie/download/details.aspx?id=48145

### None of this helps me!

Please create an issue here on github or a post on the OBS forums thread (https://obsproject.com/forum/threads/native-nowplaying-widget-for-obs.196014/) describing the problem with as much detail as possible, I will try to help you.

# Compilation from source notes:

Requires the following libraries to compile properly:

https://github.com/lingeriegoat/SpotifyReader

https://github.com/lingeriegoat/CppSpotifyReaderDLLBridge

The spotify reader DLL is written in c# because I am more comfortable working with windows using c#

Most of the cpp code is AI generated because I am awful at writing cpp. Seems to work fine though.

If you want to get the c# dll function working natively in cpp so this can be a single dll instead of 3 dlls, please feel free to fork this and let me know, id love to see that. My cpp is so bad i tried but cannot do it, even with AI help.
