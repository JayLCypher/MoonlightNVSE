#include "nvse/PluginAPI.h"
#include "nvse/CommandTable.h"
#include "nvse/GameAPI.h"
#include "nvse/GameObjects.h"
#include "nvse/SafeWrite.h"
#include "nvse/NiObjects.h"
#include "nvse/Utilities.h"
#include <internal/decoding.h>

IDebugLog		gLog("logs\\moonlightNVSE.log");
PluginHandle	g_pluginHandle = kPluginHandle_Invalid;

NVSEMessagingInterface* g_messagingInterface{};
NVSEInterface* g_nvseInterface{};
NVSECommandTableInterface* g_cmdTableInterface{};

NVSEScriptInterface* g_script{};
NVSEStringVarInterface* g_stringInterface{};
NVSEArrayVarInterface* g_arrayInterface{};
NVSEDataInterface* g_dataInterface{};
NVSESerializationInterface* g_serializationInterface{};
NVSEConsoleInterface* g_consoleInterface{};
NVSEEventManagerInterface* g_eventInterface{};

GameTimeGlobals* g_gameTimeGlobals = reinterpret_cast<GameTimeGlobals*>(0x11DE7B8);
bool (*ExtractArgsEx)(COMMAND_ARGS_EX, ...);

typedef struct HSVColor { float hue, saturation, value; } HSVColor;

// Art of Computer Programming by Knuth
bool approximatelyEqual(const float a, const float b) { return fabs(a - b) <= (fabs(a) < fabs(b) ? fabs(b) : fabs(a)) * static_cast<float>(DBL_EPSILON); }
bool essentiallyEqual(const float a, const float b) { return fabs(a - b) <= (fabs(a) > fabs(b) ? fabs(b) : fabs(a)) * static_cast<float>(DBL_EPSILON); }

NiColor HSVToRGB(HSVColor hsv) {
	NiColor RGB = {
		hsv.value,
		hsv.value,
		hsv.value,
	};
	
	hsv.saturation = hsv.saturation / 100.0f;
	hsv.value = hsv.value * 255 / 100.0f;

	if (hsv.saturation >= 0.0f) {
		auto hh = static_cast<double>(hsv.hue);
		if (hh >= 360.0) { hh = 0.0; }
		hh /= 60.0;
		const long i = static_cast<long>(hh);
		const double ff = hh - i;
		const auto p = static_cast<double>(hsv.value * (1.0f - hsv.saturation));
		const auto q = static_cast<double>(hsv.value * (1.0f - hsv.saturation * static_cast<float>(ff)));
		const auto t = static_cast<double>(hsv.value * (1.0f - hsv.saturation * (1.0f - static_cast<float>(ff))));

		switch (i) {
		case 0:
			RGB.r = hsv.value;
			RGB.g = static_cast<float>(t);
			RGB.b = static_cast<float>(p);
			break;
		case 1:
			RGB.r = static_cast<float>(q);
			RGB.g = hsv.value;
			RGB.b = static_cast<float>(p);
			break;
		case 2:
			RGB.r = static_cast<float>(p);
			RGB.g = hsv.value;
			RGB.b = static_cast<float>(t);
			break;
		case 3:
			RGB.r = static_cast<float>(p);
			RGB.g = static_cast<float>(q);
			RGB.b = hsv.value;
			break;
		case 4:
			RGB.r = static_cast<float>(t);
			RGB.g = static_cast<float>(p);
			RGB.b = hsv.value;
			break;
		default:
			RGB.r = hsv.value;
			RGB.g = static_cast<float>(p);
			RGB.b = static_cast<float>(q);
			break;
		}
	}
	RGB.r = RGB.r / 255.0f;
	RGB.g = RGB.g / 255.0f;
	RGB.b = RGB.b / 255.0f;

	if (RGB.r > 1.0f) { RGB.r = 1.0f; }
	if (RGB.g > 1.0f) { RGB.g = 1.0f; }
	if (RGB.b > 1.0f) { RGB.b = 1.0f; }

#ifdef _DEBUG
	_MESSAGE("[HSVToRGB] " "R %f, G %f, B %f", RGB.r, RGB.g, RGB.b);
#endif
	return RGB;
}

HSVColor RGBToHSV(const NiColor rgb) {
	HSVColor hsv = {};

	const float fCMax = max(max(rgb.r, rgb.g), rgb.b);
	const float fCMin = min(min(rgb.r, rgb.g), rgb.b);

	if (const float fDelta = fCMax - fCMin; fDelta > 0) {
		if (approximatelyEqual(fCMax, rgb.r)) { hsv.hue = static_cast<float>(60.0 * fmod(rgb.g - rgb.b / fDelta, 6)); } // if (fCMax == rgb.r) { hsv.hue = 60.0 * fmod(rgb.g - rgb.b / fDelta, 6); }
		else if (approximatelyEqual(fCMax, rgb.g)) { hsv.hue = 60 * ((rgb.b - rgb.r) / fDelta + 2); } // else if (fCMax == rgb.g) { hsv.hue = 60 * ((rgb.b - rgb.r) / fDelta + 2); }
		else if (approximatelyEqual(fCMax ,rgb.b)) { hsv.hue = 60 * ((rgb.r - rgb.g) / fDelta + 4); } // else if (fCMax == rgb.b) { hsv.hue = 60 * ((rgb.r - rgb.g) / fDelta + 4); }

		if (fCMax > 0) { hsv.saturation = fDelta / fCMax * 100.0f; }
		else { hsv.saturation = 0.0f; }
		hsv.value = fCMax;
	}
	else {
		hsv.hue = 0.0f;
		hsv.saturation = 0.0f;
		hsv.value = fCMax;
	}

	if (hsv.hue < 0.0f) { hsv.hue = 360.0f + hsv.hue; }
	hsv.value = hsv.value * 100.0f;
#ifdef _DEBUG
	_MESSAGE("[HextoHSV] " "H %f, S %f, V %f", static_cast<double>(hsv.hue), static_cast<double>(hsv.saturation), static_cast<double>(hsv.value));
#endif
	return hsv;
}

inline HSVColor HexToHSV(const UInt32 hexValue) {
	const NiColor RGB = {
		static_cast<float>(hexValue & 0xFF) / 255.0f,
		static_cast<float>(hexValue >> 8 & 0xFF) / 255.0f,
		static_cast<float>(hexValue >> 16 & 0xFF) / 255.0f,
	};
	return RGBToHSV(RGB);
}

inline float GetDaysPassed() {
	if (g_gameTimeGlobals->daysPassed) { return g_gameTimeGlobals->daysPassed->data; }
	return 1.0f;
}

inline float Unitize(NiPoint3* src) {
	float length = sqrt(src->x * src->x + src->y * src->y + src->z * src->z);

	if (length > 1e-06f) {
		float recip = 1.0f / length;
		src->x *= recip;
		src->y *= recip;
		src->z *= recip;
	}
	else {
		src->x = 0.0f;
		src->y = 0.0f;
		src->z = 0.0f;
		length = 0.0f;
	}

	return length;
}

float multiplier = 1;
float sunriseStart, sunriseEnd, sunsetStart, sunsetEnd;
float daysPassed;
float moonVisibility = 1;

void __fastcall SetMoonLightFNV(NiNode* object, void* dummy, NiMatrix33* position) {
	Sky* FNV_sky = Sky::Get();
	const TESClimate *climate = FNV_sky->currClimate;
	HSVColor currentColor = RGBToHSV(FNV_sky->sunDirectional);
	const TES *tes = TES::Get();
	const NiNode *FNV_weather = *reinterpret_cast<NiNode**>(0x11DEDA4);

	if (FNV_sky->masserMoon != nullptr) {
		const float gameHour = FNV_sky->gameHour;

		// Sunrise Values
		sunriseStart = ThisStdCall<float>(0x595EA0, FNV_sky);
		sunriseEnd = ThisStdCall<float>(0x595F50, FNV_sky);

		// Sunset Values
		sunsetStart = ThisStdCall<float>(0x595FC0, FNV_sky);
		sunsetEnd = ThisStdCall<float>(0x596030, FNV_sky);

		daysPassed = GetDaysPassed();
		const float phase = static_cast<float>(fmod(daysPassed, (climate->phaseLength & 0x3f) * 8)) / static_cast<float>(climate->phaseLength & 0x3f);

		moonVisibility = 1;

		if (gameHour >= sunsetEnd || gameHour < sunriseStart) {
			position = &FNV_sky->masserMoon->rootNode->m_transformLocal.m_Rotate;
			position->m_pEntry[0][0] = -(position->m_pEntry[0][0] * 0.5f);

#ifdef _DEBUG
			_MESSAGE("[Time] " "Night is in progress!");
#endif

			// Moon phase management
			if (phase > 4.25f && phase < 5.25f) { moonVisibility = 0; }
			else if (phase > 3.25f && phase < 4.25f || phase > 5.25f && phase < 6.25f) { moonVisibility = 0.3f; }
			else if (phase > 2.25f && phase < 3.25f || phase > 6.25f && phase < 7.25f) { moonVisibility = 0.5f; }
			else if (phase > 1.25f && phase < 2.25f || phase > 7.25f && phase < 8.25f || phase < 0.25f) { moonVisibility = 0.9f; }
			else if (phase > 0.25f && phase < 1.25f) { moonVisibility = 1; }

			// Night
			if (gameHour > sunsetEnd) {
				multiplier = (gameHour - sunsetEnd);
#ifdef _DEBUG
				_MESSAGE("[Time] " "Night is in progress! (First half)");
#endif
			}
			else {
				multiplier = -(gameHour - sunriseStart);
#ifdef _DEBUG
				_MESSAGE("[Time] " "Night is in progress! (Second half)");
#endif
			}
		}
		else {
			//Sunset
			if (gameHour >= sunsetStart && gameHour <= sunsetEnd) {
				multiplier = -(gameHour - sunsetEnd);
#ifdef _DEBUG 	
				_MESSAGE("[Time] " "Sunset is in progress!");
#endif
			}
			// Sunrise
			else if (gameHour >= sunriseStart && gameHour <= sunriseEnd) {
				multiplier = (gameHour - sunriseStart);
#ifdef _DEBUG
				_MESSAGE("[Time] " "Sunrise is in progress!");
#endif
			}
		}

		currentColor.value *= min(max(multiplier, 0), 1);
		FNV_sky->sunDirectional = HSVToRGB(currentColor);

#ifdef _DEBUG
		auto [r, g, b] = HSVToRGB(currentColor); // structured binding
		const TESWeather* weather = FNV_sky->currWeather;
		_MESSAGE("[Sunlight]" "Current RGB color is R: %f, G: %f, B: %f", static_cast<double>(r), static_cast<double>(g), static_cast<double>(b));
		_MESSAGE("[Sunlight]" "Current HSV color is H: %f, S: %f, V: %f", static_cast<double>(currentColor.hue), static_cast<double>(currentColor.saturation), static_cast<double>(currentColor.value));
		_MESSAGE("[Weather] " "Weather %s", weather->GetEditorID());
		_MESSAGE("[Moon]    " "Current phase is %f", static_cast<double>(phase));
		_MESSAGE("[Moon]    " "Current visibility is %f", static_cast<double>(moonVisibility));
		_MESSAGE("[Time]    " "Days passed %f", static_cast<double>(daysPassed));
		_MESSAGE("[Time]    " "Current hour is %f", static_cast<double>(gameHour));
#endif
	}
	object->m_transformLocal.m_Rotate = *position;
	
	// Fixes sky position in interiors marked as exterior. 
	// It doesn't respect the north angle offset in vanilla, making sunrise happen at south etc.
	float northAngle = 0;
	if (tes->currentInterior) { northAngle = -ThisStdCall<float>(0x555AD0, tes->currentInterior); }
	ThisStdCall(0x4A0C90, &FNV_sky->niNode004->m_transformLocal.m_Rotate, northAngle);
	ThisStdCall(0x4A0C90, &FNV_weather->m_transformLocal.m_Rotate, northAngle);
}

void __fastcall SetMoonLightGECK(NiPoint3* position) {
	const Sky_GECK *GECK_sky = Sky_GECK::Get();
	const float gameHour = GECK_sky->fCurrentGameHour;
	// Not bothering with color fade
	position->y = -position->y;
	if (GECK_sky && GECK_sky->masserMoon && gameHour >= ThisStdCall<float>(0x680460, GECK_sky) || gameHour < ThisStdCall<float>(0x6803A0, GECK_sky)) {
		const NiMatrix33* rotMatrix = &GECK_sky->masserMoon->rootNode->m_transformLocal.m_Rotate;
		position->x = -(rotMatrix->m_pEntry[0][0] * 0.5f);
		position->y = rotMatrix->m_pEntry[1][0];
		position->z = rotMatrix->m_pEntry[2][0];
	}
	Unitize(position);
}

bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
	// fill out the info structure
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "MoonlightNVSE";
	info->version = 140;

	// version checks
	if (nvse->nvseVersion < PACKED_NVSE_VERSION) {
		_ERROR("NVSE version too old (got %08X expected at least %08X)", nvse->nvseVersion, PACKED_NVSE_VERSION);
		return false;
	}
	if (!nvse->isEditor && nvse->runtimeVersion < RUNTIME_VERSION_1_4_0_525) {
		if (nvse->isNogore) {
			_ERROR("NoGore is not supported");
			return false;
		}
		_ERROR("incorrect runtime version (got %08X need at least %08X)", nvse->runtimeVersion, RUNTIME_VERSION_1_4_0_525);
		return false;
	}
	if (nvse->editorVersion < CS_VERSION_1_4_0_518) {
		_ERROR("incorrect editor version (got %08X need at least %08X)", nvse->editorVersion, CS_VERSION_1_4_0_518);
		return false;
	}

	// version checks pass
	// any version compatibility checks should be done here
	return true;
}

bool NVSEPlugin_Load(NVSEInterface* nvse) {
	_MESSAGE("MoonlightNVSE loaded!");

	g_pluginHandle = nvse->GetPluginHandle();
	g_nvseInterface = nvse;

	if (!nvse->isEditor) { WriteRelCall(0x6422EE, reinterpret_cast<UInt32>(SetMoonLightFNV)); } // FNV
	else { WriteRelCall(0x685940, reinterpret_cast<UInt32>(SetMoonLightGECK)); } // GECK

	return true;
}
