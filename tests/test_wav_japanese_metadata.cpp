// RIFF text regression. All audio below is four silent PCM frames, read only.
#include "net/library.h"
#include "meta/enrich.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>

namespace fs = std::filesystem;
using std::string;
using Entries = std::vector<std::pair<string, string>>;

static string le(unsigned value, unsigned width) {
    string out;
    for (unsigned i = 0; i < width; ++i) out += (char)(value >> (8 * i));
    return out;
}
static string chunk(const string& id, const string& value) {
    return id + le((unsigned)value.size(), 4) + value + string(value.size() & 1u, '\0');
}
static string info(const Entries& entries) {
    string data = "INFO";
    for (const auto& [key, value] : entries) data += chunk(key, value + '\0');
    return chunk("LIST", data);
}
static string id3(const std::vector<std::pair<string, std::u16string>>& fields) {
    string frames;
    for (const auto& [key, text] : fields) {
        string value = "\1\xff\xfe";
        for (char16_t c : text) value += le(c, 2);
        value += le(0, 2);
        string size = le((unsigned)value.size(), 4);
        std::reverse(size.begin(), size.end());
        frames += key + size + le(0, 2) + value;
    }
    const auto n = (unsigned)frames.size();
    string syncsafe;
    for (int shift : {21, 14, 7, 0}) syncsafe += (char)((n >> shift) & 127);
    return chunk("id3 ", string("ID3\3\0\0", 6) + syncsafe + frames);
}
static string wav(const fs::path& path, const string& chunks) {
    const string format = le(1, 2) + le(2, 2) + le(44100, 4) + le(176400, 4) + le(4, 2) + le(16, 2);
    const string bytes = chunk("RIFF", "WAVE" + chunk("fmt ", format) + chunk("data", string(16, 0)) + chunks);
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), (std::streamsize)bytes.size());
    if (!out) throw std::runtime_error("cannot create synthetic WAV");
    return rta::platform::wide_to_utf8(path.wstring());
}
int test_wav_japanese_metadata() {
    const auto dir = fs::temp_directory_path() / (L"rlplayer-jp-tags-" + std::to_wstring(GetCurrentProcessId()));
    if (!fs::create_directory(dir)) throw std::runtime_error("private WAV fixture directory already exists");
    int count = 0, failures = 0;
    auto check = [&](bool ok, const string& label) {
        ++count;
        if (!ok) { ++failures; std::cerr << "FAIL " << label << '\n'; }
    };
    const string probe = rta::audio::find_tool("ffprobe");
    check(!probe.empty(), "distribution ffprobe is available");
    _putenv_s("RLPLAYER_TAG_ONLINE", "0");
    _putenv_s("RLPLAYER_TAG_GUESS", "1");
    const string goodTitle = "日本語-曲-", goodAlbum = "日本歌曲 Disc 1";
    const string i = info({{"INAM","???-?-"},{"IPRD","???? Disc 1"},{"IART","Ensemble"},{"IGNR","Game"}});
    const string t = id3({{"TIT2",u"日本語-曲-"},{"TALB",u"日本歌曲 Disc 1"},{"TPE1",u"Ensemble"}});
    // iXML is a real automatic fallback reason in the native WAV scanner.
    const string xml = chunk("iXML","<BWFXML/>");
    for (bool reverse : {false, true}) {
        for (bool fallback : {false, true}) {
            const string label = (reverse ? "ID3-first" : "INFO-first") + string(fallback ? "/iXML" : "/PCM");
            const string name = "test-" + std::to_string(reverse) + std::to_string(fallback) + ".wav";
            const string path = wav(dir / name, (reverse ? t+i : i+t) + (fallback ? xml : ""));
            rta::meta::native::TagInfo raw;
            check(rta::meta::native::probe_file(path, &raw, nullptr) == !fallback, label+" native routing");
            if (!fallback) check(raw.title == goodTitle && raw.album == goodAlbum, label+" native fields");
            for (const char* enabled : {"0","1"}) {
                _putenv_s("RLPLAYER_NATIVE_TAGS", enabled);
                const auto x = rta::net::lib_probe_one(path,probe);
                check(x.title == goodTitle && x.album == goodAlbum && x.artist == "Ensemble", label+" library/native="+enabled);
                check(x.sampleRate == 44100 && x.bits == 16 && x.channels == 2 && !x.hasCover, label+" audio properties");
            }
            const auto m = rta::audio::probe_media(path);
            check(m.title == goodTitle && m.album == goodAlbum && m.genre == "Game", label+" details");
        }
    }
    auto testText = [&](const string& name, const string& raw, const string& expected, unsigned page = 0,
                        const string& extra = {}, const string& expectedDetails = {}) {
        // Place CSET after INFO to exercise order-independent declaration lookup.
        const string cset = page ? chunk("CSET", le(page,2)+le(0,2)+le(0,2)+le(0,2)) : "";
        const auto path = wav(dir / fs::u8path(name+".wav"), info({{"INAM",raw}}) + extra + cset);
        for (const char* en : {"0","1"}) {
            _putenv_s("RLPLAYER_NATIVE_TAGS",en);
            const auto x=rta::net::lib_probe_one(path,probe);
            check(x.title == expected, name+"/library/"+en+" ["+x.title+"]");
        }
        const auto m=rta::audio::probe_media(path);
        check(m.title == (expectedDetails.empty() ? expected : expectedDetails),name+"/details ["+m.title+"]");
    };
    testText("cp932", string("\x83\x4a\x83\x69",4), "カナ", 932);
    testText("cp1252",string("Caf\xe9",4),"Café",1252);
    testText("cp936",string("\xd6\xd0\xce\xc4",4),"中文",936);
    testText("latin1 without cset",string("Caf\xe9",4),"Café");
    testText("valid ordinary question","What is love?","What is love?");
    testText("UTF-8","Chérie 日本？","Chérie 日本？");
    testText("Album DISC 2 [07]","???-?-","???-?-");
    testText("ASCII anchors differ","Long preserved ASCII prefix??","Long preserved ASCII prefix??",0,
             id3({{"TIT2",u"Different reading in Japanese"}}), "Different reading in Japanese");
    const string undecided("\x82\xa0",2);  // CP932 'あ', and also a valid double byte in CP936.
    const string safe=rta::platform::wide_to_utf8(rta::platform::utf8_to_wide(undecided));
    check(safe!="あ" && !safe.empty() && rta::meta::cue_utf8_valid(safe),"undecided display is valid UTF-8");
    testText("ambiguous no cset",undecided,safe);
    testText("verified ID3 with unmarked bytes",undecided,"あ",0,id3({{"TIT2",u"あ"}}));
    check(!rta::meta::native::wav_info_matches_unicode("Which??","Other日本"),"different surviving ASCII cannot replace");
    check(!rta::meta::native::wav_info_matches_unicode("What??","What??"),"literal question marks are preserved");
    check(!rta::meta::native::wav_info_matches_unicode("?????",string("\xff\xff\xff\xff\xff",5)),"ill-formed alternate cannot replace");
    const fs::path media = dir / "migration-media", profile = dir / "migration-profile";
    fs::create_directory(media);
    fs::create_directory(profile);
    auto utf = [](const fs::path& p) { return rta::platform::wide_to_utf8(p.wstring()); };
    std::ofstream(profile / "rlplayer-library.conf", std::ios::binary) << utf(media) << "\n";
    {
        std::ofstream idx(profile / "rlplayer-index.tsv", std::ios::binary);
        idx << "#libIndexV\t6\n#sourceDepthV\t1\n";
        for (const char* filename : {"normal.wav", "locked.wav"}) {
            const auto p = media / filename;
            const auto original = wav(p, i+t);
            idx << original << "\t" << fs::last_write_time(p).time_since_epoch().count() << "\t" << fs::file_size(p)
                << "\t???? Disc 1\tEnsemble\tEnsemble\t???-?-\t7\t1\t2025\t44100\t16\t2\t0.001\t0\tGame\t\t0\t0\t0\ttagged\tpcm_s16le\n";
        }
    }
    auto& store = rta::meta::MetaStore::instance();
    store.setBaseDir(utf(profile / "meta"));
    rta::meta::TrackMeta locked;
    locked.path = utf(media / "locked.wav");
    locked.fields["title"] = {"???-?-","user",1,1.f};
    locked.fields["album"] = {"My archive??","user",1,1.f};
    store.save(locked);
    rta::meta::TrackMeta reread;
    check(store.load(locked.path, reread), "persist explicit user title and album");
    rta::meta::refresh_embedded_wav_text(locked.path, reread);
    check(reread.fields["title"].value=="???-?-", "cached user placeholder stays literal");
    rta::meta::TrackMeta historical;
    historical.path = utf(media / "normal.wav");
    historical.fields["title"] = {"???-?-","tag",1,1.f};
    historical.fields["album"] = {"???? Disc 1","tag",1,1.f};
    historical.fields["artist"] = {"Ensemble??","qq",1,0.8f};
    store.save(historical);
    const bool readTag = store.load(historical.path, reread);
    rta::meta::refresh_embedded_wav_text(historical.path, reread);
    check(readTag && reread.fields["title"].value==goodTitle && reread.fields["album"].value==goodAlbum &&
          reread.fields["artist"].value=="Ensemble??", "old embedded values recover; provider spelling stays intact");
    _putenv_s("RLPLAYER_LIBRARY_DIR", utf(profile).c_str());
    auto& lib = rta::net::Library::instance();
    lib.load();
    check(lib.indexNeedsRetag(),"cached placeholder requests upgrade without full rescan");
    lib.startScan(false);
    lib.joinScan();
    for (int pass=0; pass<2; ++pass) {
        if (pass) lib.load();
        check(!lib.indexNeedsRetag() && lib.trackCount()==2, "verified cached rows avoid repeated rescanning");
        for (int row=0; row<2; ++row) {
            rta::net::LibTrack x;
            check(lib.trackAt(row,x) && x.wavTextChecked,"cached row marked after actual source check");
            const bool user = x.path==locked.path;
            check(x.title==(user?"???-?-":goodTitle) && x.album==(user?"My archive??":goodAlbum), "user lock survives while other row is repaired");
            check(x.year==2025 && x.bits==16 && x.trackNo==7 && x.discNo==1, "migration preserves other indexed columns");
        }
    }
    std::error_code ec;
    fs::remove_all(dir,ec);
    std::cout << (count-failures) << "/" << count << " WAV metadata assertions passed\n";
    return failures ? 1 : 0;
}
