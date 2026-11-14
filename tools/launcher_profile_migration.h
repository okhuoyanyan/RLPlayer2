// SPDX-License-Identifier: GPL-3.0-or-later
// Portable workbench data belongs to the listener, never to an update package.
#pragma once
#include "net/update_check.h"
#include "platform/artwork_file_io.h"
#include "platform/path.h"
#include <algorithm>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace rta::launcher_profiles {
namespace fs=std::filesystem;
inline constexpr const char* filename="rlplayer-workbench-profiles.json";
struct Result {std::string status="missing";fs::path source;};

// Use the newest explicit release sibling, including a previous copy retained
// during a same-version reinstall. Do not inspect arbitrary nearby folders.
inline Result migrate(const fs::path& current,const std::string& version) {
    namespace io=rta::platform::artwork_io;
    const auto target=current/filename;
    if(io::present(target))return {"existing",{}};
    const auto currentVersion=rta::update::parse_version(version);
    if(!currentVersion.valid||currentVersion.has_pre()||currentVersion.nums.size()!=3||!io::safe(current))
        return {"failed",{}};
    struct Candidate {fs::path dir;std::vector<long long> version;bool previous=false;};
    std::vector<Candidate> candidates;
    static const std::regex pattern("^RLPlayer-([0-9]{1,9}\\.[0-9]{1,9}\\.[0-9]{1,9})-win64(\\.previous-[a-f0-9]{32})?$");
    std::error_code ec;size_t visited=0;
    for(fs::directory_iterator it(current.parent_path(),ec),end;!ec&&it!=end&&visited++<4096;it.increment(ec)) {
        if(it->path()==current||!io::safe(it->path()))continue;
        std::smatch match;const auto name=rta::platform::to_utf8(it->path().filename());
        if(!std::regex_match(name,match,pattern)||!it->is_directory(ec))continue;
        const auto parsed=rta::update::parse_version(match[1]);
        if(!parsed.valid||parsed.nums>currentVersion.nums)continue;
        candidates.push_back({it->path(),parsed.nums,match[2].matched});
    }
    std::sort(candidates.begin(),candidates.end(),[](const Candidate&a,const Candidate&b){
        if(a.version!=b.version)return a.version>b.version;
        if(a.previous!=b.previous)return !a.previous;
        return a.dir.native()>b.dir.native();
    });
    for(const auto& candidate:candidates) {
        const auto source=candidate.dir/filename;
        if(!io::present(source))continue;
        std::string bytes;
        // Preserve the complete opaque document; schema validation belongs to
        // its backend owner. A failed newest read must not import stale data.
        if(!io::read(source,4u*1024u*1024u,bytes))return {"failed",source};
        if(io::publish(target,bytes))return {"copied",source};
        // Another launcher or the backend may have created its bank meanwhile.
        return {io::present(target)?"existing":"failed",source};
    }
    return {};
}
} // namespace rta::launcher_profiles
