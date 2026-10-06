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
