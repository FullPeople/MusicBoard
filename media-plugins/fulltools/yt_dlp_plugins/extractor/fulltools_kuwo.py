"""Read Kuwo's current public song pages and anonymous audio endpoint.

No account credentials, browser cookies, or alternate region headers are used.
The provider's refusal or removal response remains a download failure.
"""
from yt_dlp.extractor.common import InfoExtractor
from yt_dlp.utils import ExtractorError


class FullToolsKuwoIE(InfoExtractor):
    IE_NAME = "musicboard:kuwo"
    _VALID_URL = r"https?://(?:www\.)?kuwo\.cn/(?:play_detail|yinyue)/(?P<id>\d+)"

    def _real_extract(self, url):
        song_id = self._match_id(url)
        webpage = self._download_webpage(
            f"https://www.kuwo.cn/play_detail/{song_id}", song_id)
        title = self._html_search_regex(r"<title>([^<]+)</title>", webpage, "song title")
        suffix = "_单曲在线试听_酷我音乐"
        if not title.endswith(suffix):
            raise ExtractorError("This song is unavailable on its public page", expected=True)
        title = title[:-len(suffix)]
        data = self._search_nuxt_data(webpage, song_id, fatal=False)
        song = data.get("songinfo", {}) if isinstance(data, dict) else {}
        duration = song.get("duration") if str(song.get("rid")) == song_id else None
        if not isinstance(duration, (int, float)) or duration <= 0:
            duration = None
        audio_url = self._download_webpage(
            "https://antiserver.kuwo.cn/anti.s", song_id,
            note="Requesting public audio",
            query={"type": "convert_url", "rid": f"MUSIC_{song_id}",
                   "format": "mp3", "br": "128kmp3", "response": "url"},
        ).strip()
        if audio_url == "IPDeny":
            raise ExtractorError("This song is blocked in this region", expected=True)
        if not audio_url.startswith(("https://", "http://")):
            raise ExtractorError("The source denied access to this song", expected=True)
        return {"id": song_id, "title": title, "url": audio_url,
                "duration": duration, "ext": "mp3", "vcodec": "none", "abr": 128}
