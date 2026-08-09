# Native OBS Plugin to Display Now Playing Widget

<img width="1073" height="1815" alt="image" src="https://github.com/user-attachments/assets/bef53d1a-0d2b-4927-ae2f-c2f2dd30fbd4" />

Vertical layout enable:

<img width="439" height="605" alt="image" src="https://github.com/user-attachments/assets/b3856bcb-d0b8-4714-8e75-7de41b475d8f" />



This plugin allows you to have a native "now playing" widget inside of OBS. No need for additional programs or web servers or browser sources or any of that stuff. This is all native inside of OBS.

Works for Spotify, YoutubeMusic desktop clients, and Apple Music. As of version 1.10, VLC player is also supported, but requires a VLC plugin to actually function. You can find the required VLC plugin here: https://github.com/spmn/vlc-win10smtc . Make sure to read the installation instructions thoroughly, as it is not installed like a "normal" VLC plugin, you need to configure it or it will not do anything.

More services may be added later if they are requested and I can reasonably get a copy of the program.

# How to use

### Release 1.8 and up

Download the latest release from the releases page. Unzip into the root of your OBS directory (usually something like `C:\Program Files\obs-studio\`). Launch OBS and add a new source, choose the new option called Now Playing Widget. Configure the settings using the built in options.

<img width="1037" height="414" alt="obsplugininstallinstructions" src="https://github.com/user-attachments/assets/2b5765cf-1afe-4489-970a-1a52e7b46357" />


### Release 1.7 and prior

If you want to use a release before 1.8, the distribution method was slightly different. All pre-1.8 releases contain a zip with only 4 files inside of it. Unzip these files directly into your plugins folder, usually something like `C:\Program Files\obs-studio\obs-plugins\64bit`

# Known limitations

Only works on windows. 

# Troubleshooting

### How do I use the image background?

The "image background" option will crop your image to the size of the widgets card, starting at the top left. So for example if your image is 600x600, and your card is 300x300, you will get the top left 1/4 of your image as the background.

To properly use the feature, you should resize your image to the same size as the card. So if your card is 300x300, your image should also be 300x300.

The intent of this feature is to let people style their widget without having to have a million different options. So if you want to theme the widget to your stream, or if youre a vtuber and want to add your avatar to the card background or something like that, this is the perfect feature, but it really does require art specifically designed for it to work best.

Example 380x100 card and its 380x100 image background:

<img width="456" height="134" alt="image" src="https://github.com/user-attachments/assets/e580d924-1498-4b38-b7d4-89cd265b691c" />

<img width="380" height="100" alt="obs plugin background test 380x100" src="https://github.com/user-attachments/assets/fc494ad3-0f5d-4852-acbb-30e196749e0f" />


### The widget is blank, or its only show the name of the widget and this weird picture of a goat!

If the preview or the live source looks like this:

<img width="1044" height="280" alt="image" src="https://github.com/user-attachments/assets/87d287a3-5623-4082-89d4-2fa2e439608a" />

or like this:

<img width="1041" height="248" alt="image" src="https://github.com/user-attachments/assets/65fccbd2-6f99-4340-82e8-cb5a73c62436" />

Then most likely Windows Defender has "blocked" the SpotifyReader.dll file. You can unblock this by going to `C:\Program Files\obs-studio\obs-plugins\64bit` (Or wherever youve installed OBS), right clicking the `SpotifyReader.dll` file, selecting `Properties`, and then clicking `Unblock`, and then `Apply`. Then close OBS and reopen it, and the plugin should work.

This is an example of what the "Unblock" option looks like:

<img width="361" height="506" alt="image" src="https://github.com/user-attachments/assets/c5a79132-ae74-483f-a440-7d6f8f754401" />

The other possibility is that you are using an unsupported music source. If this is the case, please make an issue here on git, or a post on the OBS plugin thread (https://obsproject.com/forum/threads/native-nowplaying-widget-for-obs.196014/) describing what software you are trying to use with the plugin, and where it can be downloaded, and I can look into whether its possible to add support. 

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
