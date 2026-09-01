#include <obs-module.h>
#include "html-player-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-html-player-plugin", "en-US")

bool obs_module_load(void)
{
    html_source_register();
    blog(LOG_INFO, "[obs-html-player] plugin loaded");
    return true;
}

void obs_module_unload(void)
{
    html_source_unregister();
}
