# Native OBS Plugin to Display Spotify Now Playing Data

<img width="1071" height="1806" alt="image" src="https://github.com/user-attachments/assets/845a1de9-46e6-461f-b65a-5fe6607a1770" />


This plugin allows you to have a native "now playing" widget inside of OBS. No need for additional programs or web servers or browser sources or any of that stuff. This is all native inside of OBS.

# How to use

Download the latest release from the releases page. Unzip, and put all 4 files in your OBS plugins directory. Add the new source inside, it is called SpotifyNowPlaying. Configure using the built in options.

# Known limitations

Only works on windows. 
Only works for spotify. Might add youtube music later, but other services are unlikely since im not going to pay for them and I only have spotify and youtube.

#Compilation from source notes:

Requires the following libraries to compile properly:

https://github.com/lingeriegoat/SpotifyReader
https://github.com/lingeriegoat/CppSpotifyReaderDLLBridge

The spotify reader DLL is written in c# because I am more comfortable working with windows using c#

Most of the cpp code is AI generated because I am awful at writing cpp. Seems to work fine though.

If you want to get the c# dll function working natively in cpp so this can be a single dll instead of 3 dlls, please feel free to fork this and let me know, id love to see that. My cpp is so bad i tried but cannot do it, even with AI help.
