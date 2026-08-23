If you like my plugin, please check out my stream on twitch!

https://www.twitch.tv/lingeriegoat

[<img width="600" height="225" alt="image" src="https://github.com/user-attachments/assets/a7cb5155-84c2-467d-8ad1-d9e2e48af65d" />](https://www.twitch.tv/lingeriegoat)


# NowPlaying Widget for OBS

<img width="1073" height="1815" alt="image" src="https://github.com/user-attachments/assets/bef53d1a-0d2b-4927-ae2f-c2f2dd30fbd4" />

Vertical layout enabled:

<img width="439" height="605" alt="image" src="https://github.com/user-attachments/assets/b3856bcb-d0b8-4714-8e75-7de41b475d8f" />



This plugin allows you to have a native "now playing" widget inside of OBS. No need for additional programs, web servers, browser sources, text files, or any of that stuff. Everything runs entirely inside of OBS and it automatically detects the music your playing from any supported sources.

Works for Spotify, YoutubeMusic desktop clients, and Apple Music. As of version 1.10, VLC player is also supported, but requires a VLC plugin to actually function. You can find the required VLC plugin here: https://github.com/spmn/vlc-win10smtc . Make sure to read the installation instructions thoroughly, as it is not installed like a "normal" VLC plugin, you need to configure it or it will not do anything.

More services may be added later if they are requested and I can reasonably get a copy of the program.

# How to use

### Release 1.8 and up

Download the latest release from the releases page. Unzip into the root of your OBS directory (usually something like `C:\Program Files\obs-studio\`). Launch OBS and add a new source, choose the new option called Now Playing Widget. Configure the settings using the built in options.

<img width="1037" height="414" alt="obsplugininstallinstructions" src="https://github.com/user-attachments/assets/2b5765cf-1afe-4489-970a-1a52e7b46357" />


### Release 1.7 and prior

If you want to use a release before 1.8, the distribution method was slightly different. All pre-1.8 releases contain a zip with only 4 files inside of it. Unzip these files directly into your plugins folder, usually something like `C:\Program Files\obs-studio\obs-plugins\64bit`

# Known limitations

Only works on windows. Only works for spotify, youtube music, and apple music desktop clients. Works with VLC but requires a specific plugin.

If you want more sources to be supported, please let me know what they are and I can see if its feasible to add support.

Mac and linux support are infeasible as this plugin relies on a windows subsystem which has no equivalent on either mac or linux.



# Troubleshooting


### How do I use the image background?

The "image background" option will crop your image to the size of the widgets card, starting at the top left. So for example if your image is 600x600, and your card is 300x300, you will get the top left 1/4 of your image as the background.

<img width="600" height="600" alt="spotifywidgetexample" src="https://github.com/user-attachments/assets/eb4b4651-ab44-4b31-b611-f77891497261" />

To properly use the feature, you should resize your image to the same size as the card. So if your card is 300x300, your image should also be 300x300.

The intent of this feature is to let people style their widget without having to have a million different options. So if you want to theme the widget to your stream, or if youre a vtuber and want to add your avatar to the card background or something like that, this is the perfect feature, but it really does require art specifically designed for it to work best.

Example 380x100 card and its 380x100 image background:

<img width="456" height="134" alt="image" src="https://github.com/user-attachments/assets/e580d924-1498-4b38-b7d4-89cd265b691c" />

<img width="380" height="100" alt="obs plugin background test 380x100" src="https://github.com/user-attachments/assets/fc494ad3-0f5d-4852-acbb-30e196749e0f" />



### Im using spotify and the plugin isnt detecting any music!

In spotify, make sure you have enabled the "Show desktop overlay when using media keys" option. There seems to be a bug in spotify that when this is disabled, spotify stops sending data to windows SMTC, which is what we scrape from. If you really need this option disabled, please file a bug with spotify to have this fix.

<img width="933" height="177" alt="image" src="https://github.com/user-attachments/assets/13c36a34-1730-4b71-9f6f-a12ec56532a0" />



### I want to play spotify or youtube music from my browser, will this work?

Not right now, for 2 reasons. One, there is currently a bug in firefox (https://bugzilla.mozilla.org/show_bug.cgi?id=2065866) where firefox reports a really annoying name for itself in SMTC. This makes it nearly impossible to track reliably across different peoples machines. I dont really want to add browser support and leave out the 2nd most popular browser, or implement it in a way that is only semi-functional.

The second reason is that its a really good way to shoot yourself in the foot, since browsers report ANY media, and do it in a way where the most recent media is what is usually displayed. So imagine a scenario where you have spotify open in one tab, and then in another tab you navigate to some kind of "compromising" media that is now being prominently displayed on your stream... So I am trying to think of a good way to implement this that is explicitly opt-in, and make it explicitly clear that enabling the feature has the potential to cause personal or embarrassing data to leak into your stream.


### The Artist text is very small

If you had an older version of the plugin, the font has probably defaulted now that it can be changed separately. Simply open the options and select a new font. The old default was -2px smaller than whatever the Title font is, and "Regular" weight (ie, not bold or italic).

### I am using a 1.X release, and the widget is blank, or its only show the name of the widget and this weird picture of a goat!

If the preview or the live source looks like this:

<img width="1044" height="280" alt="image" src="https://github.com/user-attachments/assets/87d287a3-5623-4082-89d4-2fa2e439608a" />

or like this:

<img width="1041" height="248" alt="image" src="https://github.com/user-attachments/assets/65fccbd2-6f99-4340-82e8-cb5a73c62436" />

Then most likely Windows Defender has "blocked" the SpotifyReader.dll file. You can unblock this by going to `C:\Program Files\obs-studio\obs-plugins\64bit` (Or wherever youve installed OBS), right clicking the `SpotifyReader.dll` file, selecting `Properties`, and then clicking `Unblock`, and then `Apply`. Then close OBS and reopen it, and the plugin should work.

This is an example of what the "Unblock" option looks like:

<img width="361" height="506" alt="image" src="https://github.com/user-attachments/assets/c5a79132-ae74-483f-a440-7d6f8f754401" />

The other possibility is that you are using an unsupported music source. If this is the case, please make an issue here on git, or a post on the OBS plugin thread (https://obsproject.com/forum/threads/native-nowplaying-widget-for-obs.196014/) describing what software you are trying to use with the plugin, and where it can be downloaded, and I can look into whether its possible to add support. 



### The plugin is not loading at all!

The most likely cause is that windows defender has blocked the DLL entirely. Go to your OBS plugins folder (by default, its `C:\Program Files\obs-studio\obs-plugins\64bit`) and look for `obs-spotify-overlay-plugin.dll`. Right click on it, select "Properties", select "Unblock", and then press apply.

If you dont see the DLL there at all, then microsoft defender or some other anti-virus has likely removed it. Look up instructions for your particular anti-virus on how to resolve this, or extract another copy of it from the zip file, paste it in this folder manually, watch for any messages from your anti-virus, then follow its instructions to have it ignore this file.



### None of this helps me!

Please create an issue here on github or a post on the OBS forums thread (https://obsproject.com/forum/threads/native-nowplaying-widget-for-obs.196014/) describing the problem with as much detail as possible, I will try to help you.



# Compilation from source notes:

Version 2.0 and up compile identically to any other OBS plugin, no special features or libraries necessary. Follow their guide for getting started with plugin development.

The basics are: clone this repo and then run `cmake --build --preset windows-x64 --config Release` in the root directory.

1.X will require additional libraries to build, or precompiled binaries.

https://github.com/lingeriegoat/SpotifyReader

https://github.com/lingeriegoat/CppSpotifyReaderDLLBridge

