import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/alientek/atk-dnesp32s3-box2-wifi"


@unittest.skipUnless(shutil.which("g++"), "g++ is required")
class Box2MusicTest(unittest.TestCase):
    def test_filename_boundary_and_stereo_downmix(self):
        driver = r'''
            #include <cassert>
            #include <string>
            #include "mp3_utils.h"
            int main() {
                using namespace box2_music;
                assert(IsMp3Filename("Artist - Song.mp3"));
                assert(IsMp3Filename("歌曲.MP3"));
                assert(IsMp3Filename(std::string(251, 'a') + ".mp3"));
                for (auto name : {"", ".mp3", "../song.mp3", "MUSIC/song.mp3",
                                  "C:\\song.mp3", "song.wav", "song.mp3/", "x\n.mp3"}) {
                    assert(!IsMp3Filename(name));
                }
                assert(!IsMp3Filename(std::string("a\0b.mp3", 7)));
                assert(!IsMp3Filename(std::string(252, 'a') + ".mp3"));
                assert(StereoToMono(32767, 32767) == 32767);
                assert(StereoToMono(-32768, -32768) == -32768);
                assert(StereoToMono(32767, -32768) == 0);
                assert(StereoToMono(1000, 3000) == 2000);
            }
        '''
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "test.cc"
            binary = Path(temp) / "test"
            source.write_text(driver)
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-I", str(BOARD), str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def compile_and_run(self, driver, *sources):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "test.cc"
            binary = Path(temp) / "test"
            source.write_text(driver)
            subprocess.run(["g++", "-std=c++23", "-Wall", "-Wextra", "-Werror",
                            "-I", str(BOARD), str(source),
                            *[str(BOARD / name) for name in sources],
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_playlist_navigation_repeat_shuffle_and_refresh(self):
        self.compile_and_run(r'''
            #include <cassert>
            #include <set>
            #include "music_playlist.h"
            int main() {
                using namespace box2_music;
                Playlist p;
                assert(!p.Advance(1, true));
                p.SetTracks({"c.mp3", "a.mp3", "b.mp3", "a.mp3"});
                assert(p.Tracks().size() == 3 && p.Current() == "a.mp3");
                assert(!p.Select("missing.mp3"));
                assert(p.Advance(1, false) && p.Current() == "b.mp3");
                assert(p.Advance(1, false) && p.Current() == "c.mp3");
                assert(!p.Advance(1, false) && p.Current() == "c.mp3");
                assert(p.Advance(1, true) && p.Current() == "a.mp3");
                assert(p.Advance(-1, true) && p.Current() == "c.mp3");
                p.SetRepeat(Repeat::One);
                assert(p.Advance(1, false) && p.Current() == "c.mp3");
                assert(p.Advance(1, true) && p.Current() == "a.mp3");
                p.SetRepeat(Repeat::All);
                p.Select("c.mp3");
                assert(p.Advance(1, false) && p.Current() == "a.mp3");
                p.SetShuffle(true, 17);
                assert(p.Current() == "a.mp3");
                // Walk back to the start of this shuffled order, then check a full cycle.
                p.SetRepeat(Repeat::Off);
                while (p.Advance(-1, false)) {}
                std::set<std::string> seen;
                do { assert(seen.insert(p.Current()).second); } while (p.Advance(1, false));
                assert(seen.size() == 3);
                auto last = p.Current();
                p.SetRepeat(Repeat::All);
                assert(p.Advance(1, false) && p.Current() != last);
                p.Select("b.mp3");
                p.SetTracks({"d.mp3", "b.mp3", "a.mp3"});
                assert(p.Current() == "b.mp3");
                p.SetShuffle(false, 0);
                assert(p.Current() == "b.mp3" && p.Index() == 1);
                p.SetTracks({});
                assert(p.Current().empty() && !p.Advance(-1, true));
                std::vector<std::string> many;
                for (int i = 0; i < 600; ++i) many.push_back(std::to_string(i) + ".mp3");
                p.SetTracks(many);
                assert(p.Tracks().size() == 512);
            }
        ''')

    def test_mp3_index_vbr_metadata_seek_and_invalid_files(self):
        self.compile_and_run(r'''
            #include <array>
            #include <cassert>
            #include <memory>
            #include <string>
            #include <vector>
            #include "mp3_index.h"
            using namespace box2_music;
            void size(std::string& out, unsigned n, bool sync) {
                for (int shift : {3, 2, 1, 0})
                    out += char((n >> (shift * (sync ? 7 : 8))) & (sync ? 127 : 255));
            }
            std::string tag(unsigned version) {
                std::string body;
                for (auto [id, text] : std::vector<std::pair<std::string, std::string>>{
                        {"TIT2", std::string("\x03") + "歌曲"},
                        {"TPE1", std::string("\x00", 1) + "caf\xe9"},
                        {"TALB", std::string("\x01\xff\xfe\x2d\x4e", 5)}}) {
                    body += id;
                    size(body, text.size(), version == 4);
                    body += std::string(2, '\0');
                    body += text;
                }
                std::string out = "ID3";
                out += char(version);
                out += std::string(2, '\0');
                size(out, body.size(), true);
                return out + body;
            }
            std::string frame(unsigned bitrate) {
                std::array<uint8_t, 4> h{0xff, 0xfb, uint8_t(bitrate << 4), 0};
                auto parsed = ParseMp3Frame(h.data());
                assert(parsed);
                std::string out(parsed->bytes, '\0');
                for (int i = 0; i < 4; ++i) out[i] = char(h[i]);
                return out;
            }
            auto inspect(const std::string& data, bool cancel = false) {
                std::unique_ptr<FILE, int(*)(FILE*)> f(tmpfile(), fclose);
                assert(f);
                assert(fwrite(data.data(), 1, data.size(), f.get()) == data.size());
                rewind(f.get());
                return InspectMp3(f.get(), [cancel] { return cancel; }, [] {});
            }
            int main() {
                uint8_t v1[]{0xff, 0xfb, 0x90, 0};
                auto h = ParseMp3Frame(v1);
                assert(h && h->sample_rate == 44100 && h->bytes == 417 && h->samples == 1152);
                uint8_t v25[]{0xff, 0xe3, 0xe8, 0xc0};
                h = ParseMp3Frame(v25);
                assert(h && h->sample_rate == 8000 && h->bytes == 1440 && h->channels == 1);
                uint8_t invalid[]{0xff, 0xfb, 0, 0};
                assert(!ParseMp3Frame(invalid));
                std::string audio;
                for (int i = 0; i < 1000; ++i) audio += frame(i % 2 ? 9 : 10);
                for (unsigned version : {3, 4}) {
                    auto result = inspect(tag(version) + audio);
                    assert(result);
                    assert(result->title == "歌曲" && result->artist == "café" && result->album == "中");
                    assert(result->duration_ms == uint64_t(1000) * 1152 * 1000 / 44100);
                    assert(result->bitrate > 140000 && result->bitrate < 145000);
                    assert(result->points.size() == 6);
                    assert(result->SeekStart(0).offset == tag(version).size());
                    auto point = result->SeekStart(14000);
                    assert(point.time_ms >= 10000 && point.time_ms <= 13000);
                    assert(point.offset > tag(version).size());
                    assert(result->SeekStart(1000).time_ms == 0);
                }
                auto footer = tag(4);
                footer[5] = 0x10;
                auto result = inspect(footer + "3DI" + std::string(7, '\0') + audio);
                assert(result && result->points.front().offset == footer.size() + 10);
                auto invalid_extension = tag(4); invalid_extension[5] = 0x40;
                assert(!inspect(invalid_extension + audio));
                std::string many_frames;
                for (int i = 0; i < 256; ++i)
                    many_frames += "XXXX" + std::string(6, '\0');
                std::string many = "ID3"; many += char(4); many += std::string(2, '\0');
                size(many, many_frames.size(), true);
                many += many_frames + audio;
                FILE* file = tmpfile(); assert(file);
                assert(fwrite(many.data(), 1, many.size(), file) == many.size());
                rewind(file);
                bool cancelled = false; unsigned yields = 0;
                auto aborted = InspectMp3(file, [&] { return cancelled; }, [&] { ++yields; cancelled = true; });
                assert(!aborted && aborted.error() == "Cancelled" && yields == 1);
                fclose(file);
                assert(!inspect(audio, true));
                assert(!inspect("garbage"));
                assert(!inspect(audio.substr(0, audio.size() - 8)));
                assert(!inspect(std::string(4097, 'x') + audio));
                auto malformed = tag(4); malformed[6] = char(0x80);
                assert(!inspect(malformed + audio));
                auto huge = tag(3); huge[6] = 127;
                assert(!inspect(huge + audio));
                auto trailing = inspect(audio + "TAG" + std::string(125, '\0'));
                assert(trailing && trailing->duration_ms == 26122);
            }
        ''', "mp3_index.cc")
