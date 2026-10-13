/*
 * Vertical+ streaming platforms for the vertical output.
 *
 * Platforms with a fixed ingest server fill it in for you (server taken from
 * OBS's own rtmp-services list, or the platform's documented endpoint).
 * Platforms that hand out a per-account or per-session server (TikTok, Kick,
 * Instagram) ask for the server URL along with the key.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <cctype>
#include <cstring>
#include <string>

namespace vplus::platforms {

struct Platform {
	const char *id;
	const char *nameKey;  /* locale key for the display name */
	const char *server;   /* nullptr = the user pastes the server URL */
	const char *helpKey;  /* locale key: where to find the server and key */
};

inline const Platform *all(size_t &count)
{
	static const Platform kPlatforms[] = {
		{"tiktok", "Platform.TikTok", nullptr, "PlatformHelp.TikTok"},
		{"youtube", "Platform.YouTube", "rtmps://a.rtmps.youtube.com:443/live2", "PlatformHelp.YouTube"},
		{"twitch", "Platform.Twitch", "rtmp://live.twitch.tv/app", "PlatformHelp.Twitch"},
		{"kick", "Platform.Kick", nullptr, "PlatformHelp.Kick"},
		{"instagram", "Platform.Instagram", nullptr, "PlatformHelp.Instagram"},
		{"facebook", "Platform.Facebook", "rtmps://rtmp-api.facebook.com:443/rtmp/", "PlatformHelp.Facebook"},
		{"custom", "Platform.Custom", nullptr, "PlatformHelp.Custom"},
	};
	count = sizeof(kPlatforms) / sizeof(kPlatforms[0]);
	return kPlatforms;
}

inline const Platform *find(const std::string &id)
{
	size_t n = 0;
	const Platform *p = all(n);
	for (size_t i = 0; i < n; i++)
		if (id == p[i].id)
			return &p[i];
	return &p[n - 1]; /* custom */
}

inline std::string trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && std::isspace((unsigned char)s[a]))
		a++;
	while (b > a && std::isspace((unsigned char)s[b - 1]))
		b--;
	return s.substr(a, b - a);
}

/* The server the vertical stream will actually use. */
inline std::string resolve_server(const std::string &platformId, const std::string &customServer)
{
	const Platform *p = find(platformId);
	return p->server ? std::string(p->server) : trim(customServer);
}

/* Basic sanity check so a pasted key or web page URL fails early. */
inline bool looks_like_ingest_url(const std::string &url)
{
	return url.rfind("rtmp://", 0) == 0 || url.rfind("rtmps://", 0) == 0;
}

} // namespace vplus::platforms
