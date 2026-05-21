/*
 * embedding_bench — Criteo trace replay + cache model (02-motivation B1/B2)
 * Trace from script/extract_criteo_trace.py only.
 * SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
 */

#include <cxxopts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <list>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint64_t kGiB = 1024ULL * 1024 * 1024;
constexpr double kMaxCdfGap = 0.01;
constexpr int kCdfPoints = 512;

uint64_t logicalRowsFromGib(double tableGib, int dim) {
    const uint64_t bytesPerRow = static_cast<uint64_t>(dim) * 4ULL;
    const uint64_t tableBytes = static_cast<uint64_t>(tableGib * static_cast<double>(kGiB));
    return tableBytes / bytesPerRow;
}

uint64_t rowsFromGib(double gib, int dim) {
    const uint64_t bytes = static_cast<uint64_t>(gib * 1024.0 * 1024.0 * 1024.0);
    return bytes / (static_cast<uint64_t>(dim) * 4ULL);
}

std::string readMetaString(const std::string& metaPath, const std::string& key, const std::string& fallback) {
    std::ifstream in(metaPath);
    if (!in) {
        return fallback;
    }
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string needle = "\"" + key + "\":";
    const auto pos = content.find(needle);
    if (pos == std::string::npos) {
        return fallback;
    }
    auto start = pos + needle.size();
    while (start < content.size() && (content[start] == ' ' || content[start] == '\t')) {
        ++start;
    }
    if (start >= content.size()) {
        return fallback;
    }
    if (content[start] == '"') {
        ++start;
        const auto end = content.find('"', start);
        if (end == std::string::npos) {
            return fallback;
        }
        return content.substr(start, end - start);
    }
    const auto end = content.find_first_of(",}\n", start);
    return content.substr(start, end - start);
}

double readMetaDouble(const std::string& metaPath, const std::string& key, double fallback) {
    const std::string s = readMetaString(metaPath, key, "");
    if (s.empty()) {
        return fallback;
    }
    try {
        return std::stod(s);
    } catch (...) {
        return fallback;
    }
}

void loadAccessCdfCsv(const std::string& path, std::vector<double>& xs, std::vector<double>& ys) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open cdf csv: " + path);
    }
    std::string header;
    std::getline(in, header);
    xs.clear();
    ys.clear();
    std::string line;
    while (std::getline(in, line)) {
        const auto comma = line.find(',');
        if (comma == std::string::npos) {
            continue;
        }
        xs.push_back(std::stod(line.substr(0, comma)));
        ys.push_back(std::stod(line.substr(comma + 1)));
    }
}

struct TraceFreqModel {
    std::unordered_set<uint32_t> hotRows;
    std::unordered_map<uint32_t, int> freqRankBucket;
};

TraceFreqModel buildTraceFreqModel(const std::vector<uint32_t>& trace, int missBuckets) {
    std::unordered_map<uint32_t, uint64_t> freq;
    freq.reserve(trace.size() / 4);
    for (uint32_t row : trace) {
        ++freq[row];
    }

    std::vector<std::pair<uint64_t, uint32_t>> ranked;
    ranked.reserve(freq.size());
    for (const auto& kv : freq) {
        ranked.push_back({kv.second, kv.first});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    TraceFreqModel model;
    const uint64_t nHotRows = std::max<uint64_t>(
        1ULL, static_cast<uint64_t>(std::ceil(0.10 * static_cast<double>(ranked.size()))));
    for (uint64_t i = 0; i < nHotRows && i < ranked.size(); ++i) {
        model.hotRows.insert(ranked[static_cast<size_t>(i)].second);
    }

    const size_t nDistinct = ranked.size();
    for (size_t i = 0; i < nDistinct; ++i) {
        const int bucket = std::min(
            missBuckets - 1, static_cast<int>(static_cast<double>(i) / static_cast<double>(nDistinct) * missBuckets));
        model.freqRankBucket[ranked[i].second] = bucket;
    }
    return model;
}

void accessCdfByFrequency(const std::vector<uint32_t>& trace, std::vector<double>& xs, std::vector<double>& ys) {
    std::unordered_map<uint32_t, uint64_t> freq;
    for (uint32_t row : trace) {
        ++freq[row];
    }
    std::vector<uint64_t> masses;
    masses.reserve(freq.size());
    for (const auto& kv : freq) {
        masses.push_back(kv.second);
    }
    std::sort(masses.begin(), masses.end(), std::greater<uint64_t>());

    const double total = static_cast<double>(trace.size());
    const size_t nDistinct = masses.size();
    xs.resize(static_cast<size_t>(kCdfPoints));
    ys.resize(static_cast<size_t>(kCdfPoints));
    for (int i = 0; i < kCdfPoints; ++i) {
        const double frac = static_cast<double>(i + 1) / static_cast<double>(kCdfPoints);
        xs[static_cast<size_t>(i)] = frac;
        const size_t cutoff =
            std::max<size_t>(1, static_cast<size_t>(std::ceil(frac * static_cast<double>(nDistinct))));
        uint64_t cum = 0;
        for (size_t j = 0; j < cutoff && j < nDistinct; ++j) {
            cum += masses[j];
        }
        ys[static_cast<size_t>(i)] = static_cast<double>(cum) / total;
    }
}

uint64_t hotRowsForMass(const std::vector<uint32_t>& trace, double mass) {
    std::unordered_map<uint32_t, uint64_t> freq;
    freq.reserve(trace.size() / 4);
    for (uint32_t row : trace) {
        ++freq[row];
    }
    std::vector<uint64_t> masses;
    masses.reserve(freq.size());
    for (const auto& kv : freq) {
        masses.push_back(kv.second);
    }
    std::sort(masses.begin(), masses.end(), std::greater<uint64_t>());
    const uint64_t total = trace.size();
    const uint64_t target = static_cast<uint64_t>(std::ceil(mass * static_cast<double>(total)));
    uint64_t cum = 0;
    uint64_t rows = 0;
    for (uint64_t m : masses) {
        cum += m;
        ++rows;
        if (cum >= target) {
            break;
        }
    }
    return std::max<uint64_t>(1ULL, rows);
}

struct SimCacheCaps {
    uint64_t l1Rows = 0;
    uint64_t l2Rows = 0;
    double pressureScale = 1.0;
    uint64_t logicalHotRows = 0;
    uint64_t traceHotRows926 = 0;
};

// Trace touches a small slot subset; scale configured caps by logical hot footprint (10% of table).
SimCacheCaps resolveSimCacheCaps(uint64_t l1Rows, uint64_t l2Rows, uint64_t nLogical,
                                 const std::vector<uint32_t>& trace, bool rawCaps) {
    SimCacheCaps out{l1Rows, l2Rows, 1.0, std::max<uint64_t>(1ULL, nLogical / 10),
                     hotRowsForMass(trace, 0.926)};
    if (rawCaps || l1Rows == 0) {
        return out;
    }
    const double logicalHot = static_cast<double>(out.logicalHotRows);
    const double traceHot = static_cast<double>(out.traceHotRows926);
    if (traceHot <= 0.0 || logicalHot <= traceHot) {
        return out;
    }
    double scale = logicalHot / traceHot;
    constexpr double kMaxPressureScale = 32.0;
    scale = std::clamp(scale, 1.0, kMaxPressureScale);
    out.pressureScale = scale;
    out.l1Rows = std::max<uint64_t>(1ULL, static_cast<uint64_t>(static_cast<double>(l1Rows) / scale));
    out.l2Rows = std::max(out.l1Rows, static_cast<uint64_t>(static_cast<double>(l2Rows) / scale));
    return out;
}

double top10AccessFracFreq(const std::vector<uint32_t>& trace) {
    std::unordered_map<uint32_t, uint64_t> freq;
    for (uint32_t row : trace) {
        ++freq[row];
    }
    std::vector<uint64_t> masses;
    masses.reserve(freq.size());
    for (const auto& kv : freq) {
        masses.push_back(kv.second);
    }
    std::sort(masses.begin(), masses.end(), std::greater<uint64_t>());
    const uint64_t nHotRows = std::max<uint64_t>(
        1ULL, static_cast<uint64_t>(std::ceil(0.10 * static_cast<double>(masses.size()))));
    uint64_t hotMass = 0;
    for (uint64_t i = 0; i < nHotRows && i < masses.size(); ++i) {
        hotMass += masses[static_cast<size_t>(i)];
    }
    return static_cast<double>(hotMass) / static_cast<double>(trace.size());
}

double maxCdfGap(const std::vector<double>& targetYs, const std::vector<double>& empiricalYs) {
    double gap = 0.0;
    const size_t n = std::min(targetYs.size(), empiricalYs.size());
    for (size_t i = 0; i < n; ++i) {
        gap = std::max(gap, std::abs(targetYs[i] - empiricalYs[i]));
    }
    return gap;
}

void writeCsv(const std::string& path, const std::vector<double>& x, const std::vector<double>& y) {
    std::ofstream out(path);
    out << "rank_frac,cum_access_frac\n";
    const size_t n = std::min(x.size(), y.size());
    for (size_t i = 0; i < n; ++i) {
        out << x[i] << ',' << y[i] << '\n';
    }
}

int runPlot(const std::string& script, const std::vector<std::string>& args) {
    std::string cmd = "python3 " + script;
    for (const auto& a : args) {
        cmd += " \"" + a + "\"";
    }
    cmd += " 2>&1";
    std::cout << "[plot] " << cmd << '\n';
    return std::system(cmd.c_str());
}

// Single-level LRU (row-granularity).
class LruCache {
public:
    explicit LruCache(uint64_t capRows) : capRows_(capRows) {}

    bool contains(uint64_t key) const { return map_.find(key) != map_.end(); }

    void touch(uint64_t key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return;
        }
        order_.splice(order_.begin(), order_, it->second);
        it->second = order_.begin();
    }

    // Insert or refresh; returns victim on eviction (if any).
    std::optional<uint64_t> insert(uint64_t key) {
        if (contains(key)) {
            touch(key);
            return std::nullopt;
        }
        order_.push_front(key);
        map_[key] = order_.begin();
        if (order_.size() <= capRows_) {
            return std::nullopt;
        }
        const uint64_t victim = order_.back();
        order_.pop_back();
        map_.erase(victim);
        return victim;
    }

    void remove(uint64_t key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return;
        }
        order_.erase(it->second);
        map_.erase(it);
    }

private:
    uint64_t capRows_;
    std::list<uint64_t> order_;
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator> map_;
};

// Exclusive L1 → L2 → L3 (CXL). Each lookup does one LRU walk.
class TieredLruCache {
public:
    static constexpr int kTierL1 = 1;
    static constexpr int kTierL2 = 2;
    static constexpr int kTierL3 = 3;

    TieredLruCache(uint64_t l1Rows, uint64_t l2Rows, bool unlimited)
        : l1_(l1Rows), l2_(l2Rows), unlimited_(unlimited) {}

    int lookup(uint64_t rowId) {
        if (unlimited_) {
            l1_.insert(rowId);
            return kTierL1;
        }
        if (l1_.contains(rowId)) {
            l1_.touch(rowId);
            return kTierL1;
        }
        if (l2_.contains(rowId)) {
            l2_.touch(rowId);
            promoteToL1(rowId);
            return kTierL2;
        }
        fillFromL3(rowId);
        return kTierL3;
    }

private:
    void promoteToL1(uint64_t rowId) {
        l2_.remove(rowId);
        if (auto victim = l1_.insert(rowId)) {
            l2_.insert(*victim);
        }
    }

    void fillFromL3(uint64_t rowId) {
        l2_.remove(rowId);
        if (auto victim = l1_.insert(rowId)) {
            l2_.insert(*victim);
        }
    }

    LruCache l1_;
    LruCache l2_;
    bool unlimited_;
};

struct BenchConfig {
    std::string phase;
    double tableTb = 0.0;
    double tableGib = 10.0;
    int dim = 128;
    double l1Gib = 1.0;
    double l2Gib = 5.0;
    double simCxlMb = 10240;
    bool cacheUnlimited = false;
    uint32_t warmupPasses = 0;
    uint64_t warmupQueries = 0;
    std::string traceIn;
    std::string outDir = "results/motivation";
    bool dumpAccessCdf = false;
    bool dumpCacheCdf = false;
    bool dumpMissVsAccess = false;
    std::string overlayTarget;
    bool rawCacheCaps = false;
};

double tableGibFromCfg(const BenchConfig& cfg) {
    if (cfg.tableGib > 0.0) {
        return cfg.tableGib;
    }
    return cfg.tableTb * 1024.0;
}

void requireCriteoMeta(const std::string& metaPath) {
    if (!std::filesystem::exists(metaPath)) {
        throw std::runtime_error("missing trace_meta.json; run script/extract_criteo_trace.py first");
    }
    if (readMetaString(metaPath, "workload", "") != "criteo") {
        throw std::runtime_error("trace_meta workload must be criteo (run script/extract_criteo_trace.py)");
    }
}

void applyTraceMetaDefaults(BenchConfig& cfg, bool l1Set, bool l2Set, bool warmupSet, bool warmupQSet) {
    const std::string metaPath = cfg.outDir + "/trace/trace_meta.json";
    requireCriteoMeta(metaPath);
    if (!l1Set) {
        cfg.l1Gib = readMetaDouble(metaPath, "l1_gib", 1.0);
    }
    if (!l2Set) {
        cfg.l2Gib = readMetaDouble(metaPath, "l2_gib", 5.0);
    }
    if (!warmupSet) {
        cfg.warmupPasses = static_cast<uint32_t>(readMetaDouble(metaPath, "warmup_passes_default", 0.0));
    }
    if (!warmupQSet && cfg.warmupQueries == 0) {
        cfg.warmupQueries = static_cast<uint64_t>(readMetaDouble(metaPath, "warmup_queries_default", 0.0));
    }
    cfg.simCxlMb = tableGibFromCfg(cfg) * 1024.0;
}

std::vector<uint32_t> loadTrace(const std::string& path, uint64_t& qOut) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open trace: " + path);
    }
    in.seekg(0, std::ios::end);
    const auto bytes = static_cast<uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    if (bytes % 4 != 0) {
        throw std::runtime_error("trace size not multiple of 4");
    }
    qOut = bytes / 4;
    std::vector<uint32_t> rows(qOut);
    in.read(reinterpret_cast<char*>(rows.data()), static_cast<std::streamsize>(bytes));
    return rows;
}

} // namespace

int phaseReplayAccess(const BenchConfig& cfg) {
    uint64_t q = 0;
    auto trace = loadTrace(cfg.traceIn, q);
    const std::string metaPath = cfg.outDir + "/trace/trace_meta.json";
    requireCriteoMeta(metaPath);

    const double top10 = top10AccessFracFreq(trace);
    std::vector<double> xs;
    std::vector<double> targetYs;
    std::vector<double> empiricalYs;
    accessCdfByFrequency(trace, xs, empiricalYs);
    loadAccessCdfCsv(cfg.outDir + "/trace/target_access_cdf.csv", xs, targetYs);

    const double cdfGap = maxCdfGap(targetYs, empiricalYs);
    const bool gate1 = cdfGap <= kMaxCdfGap;

    const std::string empCsv = cfg.outDir + "/access_skew_empirical.csv";
    writeCsv(empCsv, xs, empiricalYs);

    if (cfg.dumpAccessCdf) {
        std::string target = cfg.overlayTarget;
        if (target.empty()) {
            target = cfg.outDir + "/trace/target_access_cdf.csv";
        }
        runPlot("script/plot_motivation_cdf.py",
                {"--kind", "access", "--target", target, "--empirical", empCsv, "--out",
                 cfg.outDir + "/access_skew_cdf.png", "--title", "Access skew CDF (target vs replay)"});
    }

    std::ofstream rep(cfg.outDir + "/access_skew_report.txt");
    rep << "workload=criteo\n";
    rep << "queries=" << q << "\n";
    rep << "top10_access_frac=" << top10 << "\n";
    rep << "max_cdf_gap=" << cdfGap << "\n";
    rep << "gate1_pass=" << (gate1 ? "yes" : "no") << "\n";

    std::cout << "replay-access: top10_access_frac=" << top10 << " max_cdf_gap=" << cdfGap << " gate1="
              << (gate1 ? "PASS" : "FAIL") << '\n';
    return gate1 ? 0 : 2;
}

int phaseReplayCache(const BenchConfig& cfg) {
    uint64_t q = 0;
    auto trace = loadTrace(cfg.traceIn, q);
    const std::string metaPath = cfg.outDir + "/trace/trace_meta.json";
    requireCriteoMeta(metaPath);
    const uint64_t nLogical = logicalRowsFromGib(tableGibFromCfg(cfg), cfg.dim);

    const int missBuckets = 100;
    const TraceFreqModel freqModel = buildTraceFreqModel(trace, missBuckets);

    const uint64_t l1Configured = rowsFromGib(cfg.l1Gib, cfg.dim);
    const uint64_t l2Configured = rowsFromGib(cfg.l2Gib, cfg.dim);
    SimCacheCaps caps{l1Configured, l2Configured, 1.0, std::max<uint64_t>(1ULL, nLogical / 10), 0};
    if (cfg.cacheUnlimited) {
        caps.l1Rows = nLogical;
        caps.l2Rows = nLogical;
    } else {
        caps = resolveSimCacheCaps(l1Configured, l2Configured, nLogical, trace, cfg.rawCacheCaps);
    }

    TieredLruCache cache(caps.l1Rows, caps.l2Rows, cfg.cacheUnlimited);

    for (uint32_t p = 0; p < cfg.warmupPasses; ++p) {
        for (uint32_t row : trace) {
            cache.lookup(row);
        }
    }

    uint64_t warmupQ = cfg.warmupQueries;
    if (warmupQ == 0 && cfg.warmupPasses == 0) {
        warmupQ = q / 2;
    }
    const uint64_t measureStartFinal = (warmupQ > 0 && warmupQ < q) ? warmupQ : 0;
    const uint64_t measureQ = q - measureStartFinal;

    std::vector<int> tierHist(4, 0);
    uint64_t tier1Hits = 0;
    uint64_t tier2Hits = 0;
    uint64_t tier3Hits = 0;
    uint64_t hotQ = 0;
    uint64_t coldQ = 0;
    uint64_t hotL3 = 0;
    uint64_t coldL3 = 0;
    uint64_t totalL3 = 0;

    std::vector<uint64_t> bucketTot(missBuckets, 0);
    std::vector<uint64_t> bucketL3(missBuckets, 0);

    uint64_t idx = 0;
    for (uint32_t row : trace) {
        const uint64_t r = row;
        const int tier = cache.lookup(r);
        if (idx < measureStartFinal) {
            ++idx;
            continue;
        }

        const bool isHot = freqModel.hotRows.find(row) != freqModel.hotRows.end();
        int bucket = missBuckets - 1;
        const auto it = freqModel.freqRankBucket.find(row);
        if (it != freqModel.freqRankBucket.end()) {
            bucket = it->second;
        }
        (void)r;
        (void)nLogical;

        ++tierHist[static_cast<size_t>(tier)];
        if (tier == 1) {
            ++tier1Hits;
        } else if (tier == 2) {
            ++tier2Hits;
        } else if (tier == 3) {
            ++tier3Hits;
        }
        ++bucketTot[static_cast<size_t>(bucket)];
        if (tier == 3) {
            ++bucketL3[static_cast<size_t>(bucket)];
            ++totalL3;
        }

        if (isHot) {
            ++hotQ;
            if (tier == 3) {
                ++hotL3;
            }
        } else {
            ++coldQ;
            if (tier == 3) {
                ++coldL3;
            }
        }
        ++idx;
    }

    const double pL3 = measureQ ? static_cast<double>(tierHist[3]) / static_cast<double>(measureQ) : 0.0;
    const double l3RateHot = hotQ ? static_cast<double>(hotL3) / static_cast<double>(hotQ) : 0.0;
    const double l3RateCold = coldQ ? static_cast<double>(coldL3) / static_cast<double>(coldQ) : 0.0;
    const double l3MassCold = totalL3 ? static_cast<double>(coldL3) / static_cast<double>(totalL3) : 0.0;

    const bool gate2 =
        !cfg.cacheUnlimited && pL3 > 0.001 && l3RateCold >= 2.0 * l3RateHot && l3MassCold >= 0.60;

    std::vector<double> tierX = {1, 2, 3};
    std::vector<double> tierY;
    uint64_t cum = 0;
    for (int t = 1; t <= 3; ++t) {
        cum += tierHist[static_cast<size_t>(t)];
        tierY.push_back(measureQ ? static_cast<double>(cum) / static_cast<double>(measureQ) : 0.0);
    }

    const std::string tierCsv = cfg.outDir + "/cache_tier_cdf.csv";
    {
        std::ofstream out(tierCsv);
        out << "tier,cum_frac\n";
        for (size_t i = 0; i < tierX.size(); ++i) {
            out << tierX[i] << ',' << tierY[i] << '\n';
        }
    }

    std::vector<double> missX;
    std::vector<double> missRateY;
    for (int b = 0; b < missBuckets; ++b) {
        const double frac = static_cast<double>(b + 1) / missBuckets;
        missX.push_back(frac);
        missRateY.push_back(bucketTot[static_cast<size_t>(b)]
                                ? static_cast<double>(bucketL3[static_cast<size_t>(b)]) /
                                      static_cast<double>(bucketTot[static_cast<size_t>(b)])
                                : 0.0);
    }

    const std::string missCsv = cfg.outDir + "/miss_vs_access.csv";
    {
        std::ofstream out(missCsv);
        out << "rank_frac,l3_rate\n";
        for (size_t i = 0; i < missX.size(); ++i) {
            out << missX[i] << ',' << missRateY[i] << '\n';
        }
    }

    if (cfg.dumpCacheCdf) {
        runPlot("script/plot_motivation_cdf.py",
                {"--kind", "tier", "--tier", tierCsv, "--out", cfg.outDir + "/cache_tier_cdf.png"});
    }
    if (cfg.dumpMissVsAccess) {
        runPlot("script/plot_motivation_cdf.py",
                {"--kind", "miss", "--miss", missCsv, "--out", cfg.outDir + "/miss_vs_access_cdf.png"});
    }

    std::ofstream rep(cfg.outDir + "/cache_miss_report.txt");
    rep << "workload=criteo\n";
    rep << "cache_unlimited=" << (cfg.cacheUnlimited ? "yes" : "no") << "\n";
    rep << "table_gib=" << tableGibFromCfg(cfg) << "\n";
    rep << "l1_gib=" << cfg.l1Gib << " l2_gib=" << cfg.l2Gib << " sim_cxl_mb=" << cfg.simCxlMb << "\n";
    rep << "warmup_passes=" << cfg.warmupPasses << "\n";
    rep << "warmup_queries=" << measureStartFinal << " measure_queries=" << measureQ << "\n";
    rep << "l1_rows_configured=" << l1Configured << " l2_rows_configured=" << l2Configured << "\n";
    rep << "l1_rows=" << caps.l1Rows << " l2_rows=" << caps.l2Rows << "\n";
    rep << "logical_hot_rows=" << caps.logicalHotRows << " trace_hot_rows926=" << caps.traceHotRows926
        << " cache_pressure_scale=" << caps.pressureScale << "\n";
    const double pL1 = measureQ ? static_cast<double>(tier1Hits) / static_cast<double>(measureQ) : 0.0;
    const double pL2 = measureQ ? static_cast<double>(tier2Hits) / static_cast<double>(measureQ) : 0.0;
    rep << "p_tier1=" << pL1 << " p_tier2=" << pL2 << "\n";
    rep << "p_tier3=" << pL3 << "\n";
    rep << "l3_rate_hot=" << l3RateHot << "\n";
    rep << "l3_rate_cold=" << l3RateCold << "\n";
    rep << "l3_mass_cold_rows=" << l3MassCold << "\n";
    rep << "gate2_pass=" << (cfg.cacheUnlimited ? (pL3 < 0.01 ? "yes" : "no") : (gate2 ? "yes" : "no")) << "\n";

    std::cout << "replay-cache: warmup_passes=" << cfg.warmupPasses << " warmup_queries=" << measureStartFinal
              << " measure_q=" << measureQ << " l1_rows=" << caps.l1Rows << " l2_rows=" << caps.l2Rows
              << " pressure=" << caps.pressureScale
              << " P(tier1)=" << pL1 << " P(tier2)=" << pL2 << " P(tier3)=" << pL3 << " l3_hot=" << l3RateHot
              << " l3_cold=" << l3RateCold << " l3_mass_cold=" << l3MassCold;
    if (cfg.cacheUnlimited) {
        std::cout << " gate2_struct=" << (pL3 < 0.01 ? "PASS" : "FAIL");
    } else {
        std::cout << " gate2=" << (gate2 ? "PASS" : "FAIL");
    }
    std::cout << '\n';

    if (cfg.cacheUnlimited) {
        return pL3 < 0.01 ? 0 : 2;
    }
    return gate2 ? 0 : 2;
}

int main(int argc, char** argv) {
    BenchConfig cfg;
    cxxopts::Options opts("embedding_bench", "Criteo trace replay + cache model (02-motivation)");
    opts.add_options()("phase", "replay-access | replay-cache", cxxopts::value<std::string>())(
        "table-gib", "Logical table size (GiB); default 10", cxxopts::value<double>())(
        "table-tb", "Logical table (TiB); overrides --table-gib if set", cxxopts::value<double>())(
        "dim", "Vector dimension", cxxopts::value<int>()->default_value("128"))(
        "l1-gib", "L1 HBM cache (GiB); default from trace_meta", cxxopts::value<double>())(
        "l2-gib", "L2 DDR cache (GiB); default from trace_meta", cxxopts::value<double>())(
        "sim-cxl-mb", "Sim CXL pool (MiB); default = table-gib*1024", cxxopts::value<double>())(
        "cache-unlimited", "Infinite L1/L2", cxxopts::value<bool>()->default_value("false"))(
        "warmup-passes", "Full-trace replay passes before counting (0=off)", cxxopts::value<uint32_t>()->default_value("0"))(
        "warmup-queries", "Prefix lookups to warm LRU without counting (0=use meta or Q/2)",
        cxxopts::value<uint64_t>())("trace-in", "read_trace.bin from extract_criteo_trace.py",
                                    cxxopts::value<std::string>())(
        "out-dir", "Output directory", cxxopts::value<std::string>()->default_value("results/motivation"))(
        "dump-access-cdf", "Plot replay access CDF", cxxopts::value<bool>()->default_value("false"))(
        "overlay-target", "Target CSV for overlay", cxxopts::value<std::string>())(
        "dump-cache-cdf", "Plot tier CDF", cxxopts::value<bool>()->default_value("false"))(
        "dump-miss-vs-access", "Plot miss vs access", cxxopts::value<bool>()->default_value("false"))(
        "raw-cache-caps", "Use configured L1/L2 without logical-table pressure scaling",
        cxxopts::value<bool>()->default_value("false"))("h,help", "Print usage");

    try {
        const auto args = opts.parse(argc, argv);
        if (args.count("help")) {
            std::cout << opts.help() << std::endl;
            return 0;
        }
        if (!args.count("phase")) {
            std::cerr << "missing --phase\n" << opts.help() << std::endl;
            return 1;
        }
        cfg.phase = args["phase"].as<std::string>();
        if (args.count("table-gib")) {
            cfg.tableGib = args["table-gib"].as<double>();
        }
        if (args.count("table-tb")) {
            cfg.tableTb = args["table-tb"].as<double>();
            cfg.tableGib = cfg.tableTb * 1024.0;
        }
        cfg.dim = args["dim"].as<int>();
        const bool l1Set = args.count("l1-gib") > 0;
        const bool l2Set = args.count("l2-gib") > 0;
        if (l1Set) {
            cfg.l1Gib = args["l1-gib"].as<double>();
        }
        if (l2Set) {
            cfg.l2Gib = args["l2-gib"].as<double>();
        }
        if (args.count("sim-cxl-mb")) {
            cfg.simCxlMb = args["sim-cxl-mb"].as<double>();
        }
        cfg.cacheUnlimited = args["cache-unlimited"].as<bool>();
        const bool warmupSet = args.count("warmup-passes") > 0;
        cfg.warmupPasses = warmupSet ? args["warmup-passes"].as<uint32_t>() : 0;
        const bool warmupQSet = args.count("warmup-queries") > 0;
        if (warmupQSet) {
            cfg.warmupQueries = args["warmup-queries"].as<uint64_t>();
        }
        if (args.count("trace-in")) {
            cfg.traceIn = args["trace-in"].as<std::string>();
        }
        if (args.count("out-dir")) {
            cfg.outDir = args["out-dir"].as<std::string>();
        }
        cfg.dumpAccessCdf = args["dump-access-cdf"].as<bool>();
        cfg.overlayTarget = args.count("overlay-target") ? args["overlay-target"].as<std::string>() : "";
        cfg.dumpCacheCdf = args["dump-cache-cdf"].as<bool>();
        cfg.dumpMissVsAccess = args["dump-miss-vs-access"].as<bool>();
        cfg.rawCacheCaps = args["raw-cache-caps"].as<bool>();

        if (cfg.traceIn.empty()) {
            cfg.traceIn = cfg.outDir + "/trace/read_trace.bin";
        }

        std::filesystem::create_directories(cfg.outDir + "/trace");
        applyTraceMetaDefaults(cfg, l1Set, l2Set, warmupSet, warmupQSet);

        if (cfg.phase == "replay-access") {
            return phaseReplayAccess(cfg);
        }
        if (cfg.phase == "replay-cache") {
            return phaseReplayCache(cfg);
        }
        std::cerr << "unknown phase: " << cfg.phase << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}
