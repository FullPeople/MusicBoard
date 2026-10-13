"""Resolve single-track Kugou URLs using the provider's public playback API."""
import re
import urllib.parse

from yt_dlp.extractor.common import InfoExtractor
from yt_dlp.utils import ExtractorError


class FullToolsKugouIE(InfoExtractor):
    IE_NAME = "musicboard:kugou"
    _VALID_URL = r"https?://(?:www\.)?kugou\.com/(?:song-\d+/[a-zA-Z0-9]+\.html|song/.*)"

    def _real_extract(self, url):
        parts = urllib.parse.urlsplit(url)
        parameters = urllib.parse.parse_qs(parts.query + "&" + parts.fragment)
        song_hash = parameters.get("hash", [""])[0]
        album_id = parameters.get("album_id", [""])[0]
        metadata = {}
        if not re.fullmatch(r"[a-fA-F0-9]{32}", song_hash):
            webpage = self._download_webpage(url, "song")
            songs = self._search_json(r"\bvar dataFromSmarty\s*=", webpage,
                                      "song metadata", "song", contains_pattern=r"\[.*?\]")
            if not isinstance(songs, list) or len(songs) != 1 or not isinstance(songs[0], dict):
                raise ExtractorError("This is not a single-track song page", expected=True)
            metadata = songs[0]
            song_hash = metadata.get("hash", "")
            album_id = metadata.get("album_id", "")
        if not isinstance(song_hash, str) or not re.fullmatch(r"[a-fA-F0-9]{32}", song_hash):
            raise ExtractorError("The source did not identify this song", expected=True)
        query = {"r": "play/getdata", "hash": song_hash}
        if str(album_id).isdigit():
            query["album_id"] = str(album_id)
        response = self._download_json("https://wwwapi.kugou.com/yy/index.php",
                                       song_hash, query=query, headers={"Referer": url})
        data = response.get("data", {})
        audio_url = data.get("play_url") if isinstance(data, dict) else None
        if not isinstance(audio_url, str) or not audio_url.startswith(("http://", "https://")):
            raise ExtractorError(
                f"The source denied access to this song (error {response.get('err_code', 'unknown')})",
                expected=True,
            )
        title = data.get("audio_name") or metadata.get("audio_name")
        if not isinstance(title, str) or not title:
            raise ExtractorError("The source did not provide the song title", expected=True)
        milliseconds = metadata.get("timelength") or data.get("timelength")
        duration = milliseconds / 1000 if isinstance(milliseconds, (int, float)) and milliseconds > 0 else None
        return {"id": song_hash, "title": title, "url": audio_url,
                "duration": duration, "ext": "mp3", "vcodec": "none"}
