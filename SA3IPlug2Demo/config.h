#pragma once

#define PLUG_NAME "SA3IPlug2Demo"
#define PLUG_MFR "the collabage patch"
#define PLUG_VERSION_HEX 0x00010300
#define PLUG_VERSION_STR "0.1.3"
#define PLUG_UNIQUE_ID 'S3id'
#define PLUG_MFR_ID 'Tcpa'
#define PLUG_URL_STR "https://github.com/betweentwomidnights/sa3.cpp"
#define PLUG_EMAIL_STR "hello@example.com"
#define PLUG_COPYRIGHT_STR "Copyright 2026 The Collabage Patch"
#define PLUG_CLASS_NAME SA3IPlug2Demo

#define BUNDLE_NAME "SA3IPlug2Demo"
#define BUNDLE_MFR "the collabage patch"
#define BUNDLE_DOMAIN "com"

#define SHARED_RESOURCES_SUBPATH "SA3IPlug2Demo"

#define PLUG_CHANNEL_IO "1-1 2-2"

#define PLUG_LATENCY 0
#define PLUG_TYPE 0
#define PLUG_DOES_MIDI_IN 0
#define PLUG_DOES_MIDI_OUT 0
#define PLUG_DOES_MPE 0
#define PLUG_DOES_STATE_CHUNKS 0
#define PLUG_HAS_UI 1
#define PLUG_WIDTH 420
#define PLUG_HEIGHT 920
#define PLUG_FPS 30
#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 0

#define AUV2_ENTRY SA3IPlug2Demo_Entry
#define AUV2_ENTRY_STR "SA3IPlug2Demo_Entry"
#define AUV2_FACTORY SA3IPlug2Demo_Factory
#define AUV2_VIEW_CLASS SA3IPlug2Demo_View
#define AUV2_VIEW_CLASS_STR "SA3IPlug2Demo_View"

#define AAX_TYPE_IDS 'S3D1', 'S3D2'
#define AAX_TYPE_IDS_AUDIOSUITE 'S3A1', 'S3A2'
#define AAX_PLUG_MFR_STR "TCP"
#define AAX_PLUG_NAME_STR "SA3IPlug2Demo\nS3ID"
#define AAX_PLUG_CATEGORY_STR "Effect"
#define AAX_DOES_AUDIOSUITE 1

#define VST3_SUBCATEGORY "Fx|Generator"

#define CLAP_MANUAL_URL "https://github.com/betweentwomidnights/sa3.cpp"
#define CLAP_SUPPORT_URL "https://github.com/betweentwomidnights/sa3.cpp"
#define CLAP_DESCRIPTION "Embedded Stable Audio 3 test plugin"
#define CLAP_FEATURES "audio-effect", "utility"

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
#define APP_SIGNAL_VECTOR_SIZE 64

#define ROBOTO_FN "Roboto-Regular.ttf"
