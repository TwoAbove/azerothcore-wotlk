/*
 * mod-bot-minds — core singleton implementation. See BotMindsCore.h for the thread model.
 */

#include "BotMindsCore.h"

#include "Config.h"
#include "Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace BotMinds
{

namespace
{
constexpr std::size_t MaxOpsReadBytes = 64 * 1024;
constexpr std::size_t MaxOpLineBytes = 64 * 1024;
constexpr std::size_t MaxQueuedOpsPerBot = 256;
constexpr std::size_t MaxPerceptQueue = 100000;
constexpr uint8_t MaxPerceptWriteFailures = 3;
}

Core* Core::instance()
{
    static Core core;
    return &core;
}

uint64_t Core::NowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string Core::Esc(std::string const& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                    out += static_cast<char>(c);
        }
    }
    return out;
}

std::string Core::Base(std::string const& bot, char const* kind)
{
    std::ostringstream o;
    o << "{\"t\":" << NowMs() << ",\"bot\":\"" << Esc(bot) << "\",\"kind\":\"" << kind << "\"";
    return o.str();
}

void Core::LoadConfig(bool reload)
{
    bool enable = sConfigMgr->GetOption<bool>("BotMinds.Enable", false);
    std::string watchList = sConfigMgr->GetOption<std::string>("BotMinds.Watch", "");
    std::string dir = sConfigMgr->GetOption<std::string>("BotMinds.PerceptDir", "/home/twoabove/wow/percepts");
    uint32_t pulseMinutes = sConfigMgr->GetOption<uint32_t>("BotMinds.PulseMinutes", 5);
    uint32_t opsPollMs = sConfigMgr->GetOption<uint32_t>("BotMinds.OpsPollMs", 500);
    opsPollMs = std::clamp<uint32_t>(opsPollMs, 100, 60000);

    _pulseMinutes.store(pulseMinutes, std::memory_order_relaxed);
    _opsPollMs.store(opsPollMs, std::memory_order_relaxed);

    std::unordered_set<std::string> watch;
    std::stringstream ss(watchList);
    std::string name;
    while (std::getline(ss, name, ','))
    {
        size_t b = name.find_first_not_of(" \t");
        size_t e = name.find_last_not_of(" \t");
        if (b == std::string::npos)
            continue;
        watch.insert(name.substr(b, e - b + 1));
    }
    if (watch.empty())
        enable = false;

    {
        std::lock_guard<std::mutex> lock(_mx);
        bool const sourceChanged = _perceptDir != dir;
        for (std::string const& bot : _watch)
        {
            if (sourceChanged || watch.count(bot) == 0)
            {
                _opsResetBots.insert(bot);
                _opQ.erase(bot);
            }
        }
        if (sourceChanged)
        {
            for (std::string const& bot : watch)
            {
                _opsResetBots.insert(bot);
                _opQ.erase(bot);
            }
        }
        _watch = std::move(watch);
        _perceptDir = dir;
    }

    if (enable)
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec)
        {
            LOG_ERROR("modules", "BotMinds: cannot create PerceptDir '{}': {} — disabled", dir, ec.message());
            enable = false;
        }
    }

    _enabled.store(enable, std::memory_order_relaxed);

    if (enable && !_running.load())
    {
        _running.store(true);
        _worker = std::thread(&Core::WorkerLoop, this);
        LOG_INFO("modules", "BotMinds: enabled, watching [{}], percepts -> {}", watchList, dir);
    }
    else if (!enable && _running.load())
    {
        Shutdown();
        LOG_INFO("modules", "BotMinds: disabled by config reload");
    }
    else if (enable)
    {
        LOG_INFO("modules", "BotMinds: config reloaded ({}), watching [{}]", reload ? "reload" : "boot", watchList);
    }
}

void Core::Shutdown()
{
    if (!_running.load())
        return;
    _running.store(false);
    _workerCv.notify_all();
    if (_worker.joinable())
        _worker.join();
    FlushPercepts(); // final drain
    {
        std::lock_guard<std::mutex> lock(_mx);
        _opQ.clear();
        _opsResetBots.clear();
    }
    _opsOffsets.clear();
    _opsPartial.clear();
    _opsDroppingLine.clear();
}

bool Core::IsWatched(std::string const& name)
{
    if (!Enabled())
        return false;
    std::lock_guard<std::mutex> lock(_mx);
    return _watch.count(name) != 0;
}

void Core::Push(std::string const& bot, std::string&& line)
{
    std::lock_guard<std::mutex> lock(_mx);
    _outQ.push_back({bot, std::move(line), 0});
    if (_outQ.size() > MaxPerceptQueue)
        _outQ.pop_front();
}

bool Core::PopOp(std::string const& bot, Op& out)
{
    std::lock_guard<std::mutex> lock(_mx);
    auto it = _opQ.find(bot);
    if (it == _opQ.end() || it->second.empty())
        return false;
    out = std::move(it->second.front());
    it->second.pop_front();
    return true;
}

void Core::FlushPercepts()
{
    std::deque<PendingPercept> local;
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(_mx);
        local.swap(_outQ);
        dir = _perceptDir;
    }
    if (local.empty())
        return;

    std::unordered_map<std::string, std::vector<PendingPercept>> batches;
    for (PendingPercept& percept : local)
        batches[percept.bot].push_back(std::move(percept));

    for (auto& [bot, percepts] : batches)
    {
        std::string data;
        for (PendingPercept const& percept : percepts)
        {
            data += percept.line;
            data += '\n';
        }

        std::string const path = dir + "/" + bot + ".percepts.jsonl";
        std::ofstream f(path, std::ios::app | std::ios::binary);
        if (f)
        {
            f.write(data.data(), static_cast<std::streamsize>(data.size()));
            f.close();
        }
        if (!f.fail())
            continue;

        std::size_t requeued = 0;
        std::size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(_mx);
            for (auto it = percepts.rbegin(); it != percepts.rend(); ++it)
            {
                if (++it->failures >= MaxPerceptWriteFailures)
                {
                    ++dropped;
                    continue;
                }
                if (_outQ.size() >= MaxPerceptQueue)
                    _outQ.pop_back();
                _outQ.emplace_front(std::move(*it));
                ++requeued;
            }
        }
        LOG_WARN("modules", "BotMinds: failed to write percept file '{}'; requeued {}, dropped {} after {} failures",
            path, requeued, dropped, MaxPerceptWriteFailures);
    }
}

bool Core::ParseOpLine(std::string const& line, Op& op, std::string& error)
{
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object())
    {
        error = "unparseable json";
        return false;
    }

    auto kind = j.find("op");
    if (kind == j.end() || !kind->is_string())
    {
        error = "missing op kind";
        return false;
    }
    op.kind = kind->get<std::string>();

    auto id = j.find("id");
    if (id != j.end() && id->is_string())
        op.id = id->get<std::string>();

    if (op.kind == "state")
        return true;

    auto text = j.find("text");
    bool const hasText =
        text != j.end() && text->is_string() && !text->get_ref<std::string const&>().empty();

    if (op.kind == "command")
    {
        if (!hasText)
        {
            error = "command without text";
            return false;
        }
        op.text = text->get<std::string>();
        return true;
    }

    if (op.kind != "say")
    {
        error = "unsupported op '" + op.kind + "'";
        return false;
    }

    if (!hasText)
    {
        error = "say without text";
        return false;
    }

    auto channel = j.find("channel");
    if (channel != j.end() && !channel->is_string())
    {
        error = "say with invalid channel";
        return false;
    }

    auto to = j.find("to");
    if (to != j.end() && !to->is_string())
    {
        error = "say with invalid target";
        return false;
    }

    op.channel = channel == j.end() ? "say" : channel->get<std::string>();
    op.to = to == j.end() ? "" : to->get<std::string>();
    op.text = text->get<std::string>();
    return true;
}

void Core::TailOps()
{
    std::vector<std::string> bots;
    std::vector<std::string> resetBots;
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(_mx);
        bots.assign(_watch.begin(), _watch.end());
        resetBots.assign(_opsResetBots.begin(), _opsResetBots.end());
        _opsResetBots.clear();
        dir = _perceptDir;
    }
    for (std::string const& bot : resetBots)
    {
        _opsOffsets.erase(bot);
        _opsPartial.erase(bot);
        _opsDroppingLine.erase(bot);
    }

    for (std::string const& bot : bots)
    {
        std::string const path = dir + "/" + bot + ".ops.jsonl";
        std::error_code ec;
        if (!fs::exists(path, ec) || ec)
            continue;
        uint64_t const size = static_cast<uint64_t>(fs::file_size(path, ec));
        if (ec)
            continue;

        auto [it, inserted] = _opsOffsets.try_emplace(bot, size);
        if (inserted)
            continue; // first sighting: start at EOF, never replay history
        uint64_t& off = it->second;
        std::string& partial = _opsPartial[bot];
        if (size < off)
        {
            off = 0; // truncated — restart without joining two partial lines
            partial.clear();
            _opsDroppingLine.erase(bot);
        }
        if (size == off)
            continue;

        std::ifstream f(path, std::ios::binary);
        if (!f)
            continue;
        f.seekg(static_cast<std::streamoff>(off));
        if (!f)
            continue;

        std::size_t const readSize = static_cast<std::size_t>(
            std::min<uint64_t>(size - off, MaxOpsReadBytes));
        std::string chunk(readSize, '\0');
        f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        std::streamsize const bytesRead = f.gcount();
        if (bytesRead <= 0)
            continue;
        chunk.resize(static_cast<std::size_t>(bytesRead));
        off += static_cast<uint64_t>(bytesRead);

        std::size_t droppedOps = 0;
        std::size_t cursor = 0;
        while (cursor < chunk.size())
        {
            if (_opsDroppingLine.count(bot) != 0)
            {
                std::size_t const nl = chunk.find('\n', cursor);
                if (nl == std::string::npos)
                    break;
                _opsDroppingLine.erase(bot);
                cursor = nl + 1;
                continue;
            }

            std::size_t const nl = chunk.find('\n', cursor);
            std::size_t const end = nl == std::string::npos ? chunk.size() : nl;
            std::size_t const segmentSize = end - cursor;
            if (partial.size() + segmentSize > MaxOpLineBytes)
            {
                partial.clear();
                LOG_WARN("modules", "BotMinds: dropping oversized op line for '{}' (maximum {} bytes)",
                    bot, MaxOpLineBytes);
                if (nl == std::string::npos)
                {
                    _opsDroppingLine.insert(bot);
                    break;
                }
                cursor = nl + 1;
                continue;
            }

            partial.append(chunk.data() + cursor, segmentSize);
            if (nl == std::string::npos)
                break;
            cursor = nl + 1;

            std::string line;
            line.swap(partial);
            if (line.find_first_not_of(" \t\r") == std::string::npos)
                continue;

            bool sourceCurrent;
            {
                std::lock_guard<std::mutex> lock(_mx);
                sourceCurrent = _perceptDir == dir && _watch.count(bot) != 0;
            }
            if (!sourceCurrent)
                continue;

            Op op;
            std::string error;
            if (!ParseOpLine(line, op, error))
            {
                std::string p = Base(bot, "task");
                p += ",\"status\":\"rejected\",\"text\":\"" + Esc(line) + "\",\"reason\":\"" + Esc(error) + "\"}";
                Push(bot, std::move(p));
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(_mx);
                if (_perceptDir != dir || _watch.count(bot) == 0)
                    continue;
                std::deque<Op>& queue = _opQ[bot];
                if (queue.size() >= MaxQueuedOpsPerBot)
                {
                    queue.pop_front();
                    ++droppedOps;
                }
                queue.push_back(std::move(op));
            }
        }
        if (droppedOps != 0)
        {
            LOG_WARN("modules", "BotMinds: op queue for '{}' reached {}; dropped {} oldest op(s)",
                bot, MaxQueuedOpsPerBot, droppedOps);
        }
    }
}

void Core::WorkerLoop()
{
    LOG_INFO("modules", "BotMinds: IO worker started");
    std::unique_lock<std::mutex> lock(_workerMx);
    while (_running.load())
    {
        _workerCv.wait_for(lock, std::chrono::milliseconds(_opsPollMs.load(std::memory_order_relaxed)),
            [this]() { return !_running.load(); });
        if (!_running.load())
            break;
        lock.unlock();
        FlushPercepts();
        TailOps();
        lock.lock();
    }
    LOG_INFO("modules", "BotMinds: IO worker exited");
}

} // namespace BotMinds
