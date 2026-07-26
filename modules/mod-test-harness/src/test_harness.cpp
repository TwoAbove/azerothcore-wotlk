/*
 * Reusable headless integration-test harness.
 *
 * The actor is a real Player loaded through AzerothCore's login query holder,
 * but its WorldSession has no socket and is not registered with playerbots.
 * Player dumps provide durable, whole-character transactions and crash recovery.
 */
#include "test_harness.h"

#include "AccountMgr.h"
#include "AchievementMgr.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "MailMgr.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PassiveAI.h"
#include "Player.h"
#include "ReputationMgr.h"
#include "PlayerDump.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "TemporarySummon.h"
#include "ThreatManager.h"
#include "Tokenize.h"
#include "UpdateFields.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace TestHarness
{
namespace
{
struct Settings
{
    bool enabled = true;
    bool autoStart = true;
    std::string account = "HARNESS_ACTOR";
    std::string actorName = "Tertiarytest";
    uint8 race = RACE_HUMAN;
    uint8 playerClass = CLASS_MAGE;
    uint8 gender = GENDER_MALE;
    uint8 level = 80;
    uint32 map = 0;
    float x = -8949.95f;
    float y = -132.493f;
    float z = 83.5312f;
    float o = 0.0f;
    uint32 phaseMask = PHASEMASK_NORMAL;
    std::vector<uint32> spells;
    uint32 dummyEntry = 118; // Prowler: plain attackable beast, no script/AI name
    uint32 dummyFaction = 14;
    uint32 dummyHealth = 1000000;
    std::filesystem::path stateDirectory = "test-harness";
    uint32 transactionTimeoutSeconds = 300;
    std::size_t maxEvents = 4096;
};

struct Fingerprint
{
    uint64 value = 1469598103934665603ULL;
    std::string summary;

    bool operator==(Fingerprint const& other) const
    {
        return value == other.value && summary == other.summary;
    }
};

struct PendingReport
{
    std::string suite;
    std::vector<Assertion> assertions;
    uint32 durationMs = 0;
    bool executionPassed = false;
    bool transactional = true;
    std::string executionError;
};
struct GroupRun
{
    std::string name;
    std::vector<std::string> suites;
    std::size_t next = 0;
    std::size_t passed = 0;
    std::size_t failed = 0;
    uint64 durationMs = 0;
};


std::map<std::string, SuiteFactory, std::less<>>& SuiteRegistry()
{
    static std::map<std::string, SuiteFactory, std::less<>> registry;
    return registry;
}
std::map<std::string, std::vector<std::string>, std::less<>>& SuiteGroupRegistry()
{
    static std::map<std::string, std::vector<std::string>, std::less<>> registry;
    return registry;
}

constexpr std::string_view SNAPSHOT_PREFIX = "TEST HARNESS SNAPSHOT ";

struct SnapshotParticipant
{
    SnapshotCapture capture;
    SnapshotRestore restore;
};

std::map<std::string, SnapshotParticipant, std::less<>>& SnapshotParticipantRegistry()
{
    static std::map<std::string, SnapshotParticipant, std::less<>> registry;
    return registry;
}


std::string EventTypeName(EventType type)
{
    switch (type)
    {
        case EventType::CastCheck: return "cast-check";
        case EventType::Cast: return "cast";
        case EventType::CastCancel: return "cast-cancel";
        case EventType::Damage: return "damage";
        case EventType::DamageFinal: return "damage-final";
        case EventType::Heal: return "heal";
        case EventType::SpellDamage: return "spell-damage";
        case EventType::PeriodicDamage: return "periodic-damage";
        case EventType::SpellHeal: return "spell-heal";
    }

    return "unknown";
}

void HashValue(uint64& hash, uint64 value)
{
    constexpr uint64 FNV_PRIME = 1099511628211ULL;
    for (uint8 i = 0; i < sizeof(value); ++i)
    {
        hash ^= value & 0xFF;
        hash *= FNV_PRIME;
        value >>= 8;
    }
}

std::string RandomPassword()
{
    constexpr std::string_view alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    std::random_device source;
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::string password(16, 'A');
    for (char& character : password)
        character = alphabet[pick(source)];
    return password;
}

std::string GuidText(ObjectGuid guid)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << guid.GetRawValue();
    return stream.str();
}
}

class Manager
{
public:
    static Manager& Instance()
    {
        static Manager instance;
        return instance;
    }

    void LoadConfig(bool reload)
    {
        Settings next;
        next.enabled = sConfigMgr->GetOption<bool>("TestHarness.Enable", true);
        next.autoStart = sConfigMgr->GetOption<bool>("TestHarness.AutoStart", true);
        next.account = sConfigMgr->GetOption<std::string>("TestHarness.Account", "HARNESS_ACTOR");
        next.actorName = sConfigMgr->GetOption<std::string>("TestHarness.ActorName", "Tertiarytest");
        next.race = sConfigMgr->GetOption<uint8>("TestHarness.ActorRace", RACE_HUMAN);
        next.playerClass = sConfigMgr->GetOption<uint8>("TestHarness.ActorClass", CLASS_MAGE);
        next.gender = sConfigMgr->GetOption<uint8>("TestHarness.ActorGender", GENDER_MALE);
        next.level = sConfigMgr->GetOption<uint8>("TestHarness.ActorLevel", 80);
        next.map = sConfigMgr->GetOption<uint32>("TestHarness.Map", 0);
        next.x = sConfigMgr->GetOption<float>("TestHarness.X", -8949.95f);
        next.y = sConfigMgr->GetOption<float>("TestHarness.Y", -132.493f);
        next.z = sConfigMgr->GetOption<float>("TestHarness.Z", 83.5312f);
        next.o = sConfigMgr->GetOption<float>("TestHarness.O", 0.0f);
        next.phaseMask = sConfigMgr->GetOption<uint32>("TestHarness.PhaseMask", PHASEMASK_NORMAL);
        next.dummyEntry = sConfigMgr->GetOption<uint32>("TestHarness.DummyEntry", 118);
        next.dummyFaction = sConfigMgr->GetOption<uint32>("TestHarness.DummyFaction", 14);
        next.dummyHealth = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("TestHarness.DummyHealth", 1000000));
        next.stateDirectory = sConfigMgr->GetOption<std::string>("TestHarness.StateDirectory", "test-harness");
        next.transactionTimeoutSeconds = sConfigMgr->GetOption<uint32>("TestHarness.TransactionTimeoutSeconds", 300);
        next.maxEvents = std::max<std::size_t>(1, sConfigMgr->GetOption<uint32>("TestHarness.MaxEvents", 4096));

        std::string spells = sConfigMgr->GetOption<std::string>("TestHarness.Spells", "116,133,1449,2139,12051");
        for (std::string_view token : Acore::Tokenize(spells, ',', false))
            if (Optional<uint32> spell = Acore::StringTo<uint32>(token))
                next.spells.push_back(*spell);

        bool disableRequested = reload && _settings.enabled && !next.enabled;
        bool identityProvisioned = _accountId || _guid || _session || _loading || _creating
            || _restore || _transactionActive;
        if (reload && identityProvisioned
            && (next.account != _settings.account || next.actorName != _settings.actorName
                || next.stateDirectory != _settings.stateDirectory))
        {
            LOG_ERROR("module", "TestHarness: account, actor name, and state directory cannot change after the harness identity is provisioned");
            next.account = _settings.account;
            next.actorName = _settings.actorName;
            next.stateDirectory = _settings.stateDirectory;
        }

        if (disableRequested)
            next.enabled = true;

        _settings = std::move(next);
        RefreshPaths();

        if (_events.size() > _settings.maxEvents)
        {
            std::size_t trimmed = _events.size() - _settings.maxEvents;
            _events.erase(_events.begin(), _events.begin() + trimmed);
            _droppedEvents += trimmed;
        }

        if (disableRequested)
            RequestDisable("disabled by configuration reload", false);
    }

    void Startup()
    {
        if (!_settings.enabled)
        {
            LOG_INFO("module", "TestHarness: disabled");
            return;
        }

        std::error_code error;
        std::filesystem::create_directories(_settings.stateDirectory, error);
        if (error)
        {
            Disable("cannot create state directory " + _settings.stateDirectory.string() + ": " + error.message());
            return;
        }

        _accountId = AccountMgr::GetId(_settings.account);
        if (!_accountId)
        {
            AccountOpResult result = sAccountMgr->CreateAccount(_settings.account, RandomPassword());
            if (result != AOR_OK && result != AOR_NAME_ALREADY_EXIST)
            {
                Disable("cannot create harness account, result=" + std::to_string(result));
                return;
            }

            _waitingForAccount = true;
            _provisionElapsed = 0;
            _accountPollElapsed = 0;
            LOG_INFO("module", "TestHarness: dedicated account creation queued for {}", _settings.account);
            return;
        }

        ContinueStartupAfterAccount();
    }

    void Shutdown()
    {
        CancelRun("world shutdown");
        DespawnAllDummies();
        StopActorInternal();
        StopCreationSession();
    }

    void Update(uint32 diff)
    {
        if (!_settings.enabled)
            return;

        if (_waitingForAccount)
        {
            _provisionElapsed += diff;
            _accountPollElapsed += diff;
            if (_accountPollElapsed >= 250)
            {
                _accountPollElapsed = 0;
                _accountId = AccountMgr::GetId(_settings.account);
                if (_accountId)
                {
                    _waitingForAccount = false;
                    LOG_INFO("module", "TestHarness: provisioned dedicated account {} ({})",
                        _settings.account, _accountId);
                    ContinueStartupAfterAccount();
                }
            }

            if (_waitingForAccount && _provisionElapsed >= 30000)
            {
                _waitingForAccount = false;
                Disable("dedicated account creation timed out");
            }
        }

        if (_creationSession)
        {
            MapSessionFilter filter(_creationSession.get());
            _creationSession->Update(diff, filter);
        }

        if (_creating)
        {
            _provisionElapsed += diff;
            if (ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(_settings.actorName))
            {
                _guid = guid;
                _creating = false;
                StopCreationSession();
                LOG_INFO("module", "TestHarness: provisioned actor {} ({})", _settings.actorName, _guid.GetCounter());
                CompleteStartup();
            }
            else if (_provisionElapsed >= 30000)
            {
                _creating = false;
                StopCreationSession();
                Disable("character provisioning timed out");
            }
        }

        if (_restore)
            UpdateRestore(diff);

        if (Player* actor = GetActor())
            AcknowledgeTeleport(actor);

        if (_initializing)
            FinishActorInitialization(diff);

        if (_transactionActive && !_restore && _settings.transactionTimeoutSeconds
            && uint64(GameTime::GetGameTimeMS().count()) - _transactionStartedAt
                >= uint64(_settings.transactionTimeoutSeconds) * IN_MILLISECONDS)
        {
            LOG_ERROR("module", "TestHarness: transaction timed out; rolling back");
            CancelRun("transaction timeout");
            RollbackTransaction(true);
        }

        UpdateRun(diff);
        FinishDisableIfReady();
    }

    Player* GetActor() const
    {
        if (!_session)
            return nullptr;

        Player* actor = _session->GetPlayer();
        return actor && actor->GetGUID() == _guid ? actor : nullptr;
    }

    bool StartActor(std::string& message)
    {
        if (!_settings.enabled)
        {
            message = "harness disabled";
            return false;
        }

        if (_restore)
        {
            message = "actor restore is in progress";
            return false;
        }

        if (GetActor())
        {
            message = "actor already online";
            return true;
        }

        if (_creating)
        {
            message = "actor provisioning is still in progress";
            return false;
        }

        if (_loading)
        {
            message = "actor login is already in progress";
            return true;
        }

        if (!_guid)
        {
            message = "actor record is unavailable";
            return false;
        }

        if (ObjectAccessor::FindConnectedPlayer(_guid))
        {
            message = "actor is connected through a session not owned by the harness";
            return false;
        }

        std::shared_ptr<LoginQueryHolder> holder = std::make_shared<LoginQueryHolder>(_accountId, _guid);
        if (!holder->Initialize())
        {
            message = "failed to initialize actor login query holder";
            return false;
        }

        _loading = true;
        sWorld->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete(
            [](SQLQueryHolderBase const& queryHolder)
            {
                Manager::Instance().FinishActorLogin(static_cast<LoginQueryHolder const&>(queryHolder));
            });

        message = "actor login queued";
        return true;
    }

    bool StopActor(std::string& message)
    {
        if (_run || _pendingReport)
        {
            message = "cannot stop the actor while a suite or rollback verification is active";
            return false;
        }

        if (_transactionActive)
        {
            bool restored = RollbackTransaction(false);
            message = restored ? "transaction rolled back; actor stopped" : "transaction rollback failed";
            return restored;
        }

        if (_loading)
        {
            message = "actor login is in progress";
            return false;
        }

        if (!GetActor())
        {
            message = "actor already offline";
            return true;
        }

        CancelRun("actor stopped");
        DespawnAllDummies();
        StopActorInternal();
        message = "actor stopped without saving mutable test state";
        return true;
    }

    bool ResetActor(std::string& message)
    {
        if (_run || _pendingReport)
        {
            message = "cannot reset the actor while a suite or rollback verification is active";
            return false;
        }
        if (_restore)
        {
            message = "actor restore is already in progress";
            return false;
        }

        if (!std::filesystem::exists(_baselinePath))
        {
            message = "baseline dump does not exist yet";
            return false;
        }

        CancelRun("actor reset");
        DiscardTransactionArtifacts();
        bool restart = GetActor() || _settings.autoStart;
        if (!QueueRestore(_baselinePath, _accountId, _guid.GetCounter(), _settings.actorName,
            restart, RestoreKind::Baseline))
        {
            message = "baseline restore could not be queued";
            return false;
        }

        message = restart ? "baseline restore queued; actor will restart" : "baseline restore queued; actor remains offline";
        return true;
    }

    bool BeginTransaction(std::string reason, std::string& message)
    {
        Player* actor = GetActor();
        if (!actor)
        {
            message = "actor is offline";
            return false;
        }

        if (_transactionActive || std::filesystem::exists(_markerPath))
        {
            message = "a transaction is already active";
            return false;
        }

        DespawnAllDummies();
        ClearEvents();
        SaveActorSynchronously(*actor);

        if (!WriteDump(_transactionPath, _guid.GetCounter()))
        {
            message = "failed to write transaction dump";
            return false;
        }

        _transactionFingerprint = FingerprintActor(*actor);

        std::filesystem::path temporaryMarker = _markerPath;
        temporaryMarker += ".tmp";
        std::error_code ignored;
        std::filesystem::remove(temporaryMarker, ignored);

        std::ofstream marker(temporaryMarker, std::ios::trunc);
        if (!marker)
        {
            std::filesystem::remove(_transactionPath, ignored);
            message = "failed to write transaction marker";
            return false;
        }

        marker << _accountId << ' ' << _guid.GetCounter() << ' ' << _settings.actorName << '\n'
               << _transactionFingerprint.value << '\n'
               << _transactionFingerprint.summary << '\n'
               << reason << '\n';
        marker.flush();
        if (!marker)
        {
            marker.close();
            std::filesystem::remove(temporaryMarker, ignored);
            std::filesystem::remove(_transactionPath, ignored);
            message = "failed to flush transaction marker";
            return false;
        }
        marker.close();

        std::filesystem::rename(temporaryMarker, _markerPath, ignored);
        if (ignored)
        {
            std::filesystem::remove(temporaryMarker, ignored);
            std::filesystem::remove(_transactionPath, ignored);
            message = "failed to publish transaction marker";
            return false;
        }

        _transactionActive = true;
        _transactionStartedAt = GameTime::GetGameTimeMS().count();
        _transactionReason = std::move(reason);
        message = "transaction snapshot created";
        LOG_INFO("module", "TestHarness: transaction begin reason={} actor={} fingerprint={}",
            _transactionReason, _settings.actorName, _transactionFingerprint.value);
        return true;
    }

    bool CommitTransaction(std::string& message)
    {
        Player* actor = GetActor();
        if (!_transactionActive || !actor)
        {
            message = "no active transaction with an online actor";
            return false;
        }

        if (_run)
        {
            message = "runner-owned transactions cannot be committed";
            return false;
        }

        DespawnAllDummies();
        SaveActorSynchronously(*actor);
        if (!WriteDump(_baselinePath, _guid.GetCounter()))
        {
            message = "state saved, but replacing the golden baseline failed; transaction remains active";
            return false;
        }

        DiscardTransactionArtifacts();
        message = "transaction committed and golden baseline replaced";
        LOG_INFO("module", "TestHarness: transaction committed actor={}", _settings.actorName);
        return true;
    }

    bool RollbackTransaction(bool restart)
    {
        if (_restore)
            return true;

        if (_run)
            CancelRun("transaction rolled back");

        if (!_transactionActive && !std::filesystem::exists(_markerPath))
            return true;

        uint32 accountId = _accountId;
        ObjectGuid::LowType guid = _guid.GetCounter();
        std::string name = _settings.actorName;
        if (std::filesystem::exists(_markerPath) && !ReadMarker(accountId, guid, name))
        {
            LOG_ERROR("module", "TestHarness: transaction marker is malformed; refusing an unsafe restore");
            return false;
        }

        bool shouldRestart = restart && (GetActor() || _settings.autoStart || _pendingReport.has_value());
        DespawnAllDummies();
        bool restored = QueueRestore(_transactionPath, accountId, guid, name, shouldRestart, RestoreKind::Transaction);
        if (!restored)
            return false;

        LOG_INFO("module", "TestHarness: transaction restore queued actor={} expected_fingerprint={}",
            name, _transactionFingerprint.value);
        return true;
    }

    Creature* SpawnDummy(float distance, float angleOffset, uint32 entry)
    {
        Player* actor = GetActor();
        if (!actor || !actor->IsInWorld() || !actor->GetMap())
            return nullptr;

        if (!entry)
            entry = _settings.dummyEntry;
        if (!sObjectMgr->GetCreatureTemplate(entry))
            return nullptr;

        Position position = actor->GetNearPosition(std::max(1.0f, distance), angleOffset);
        position.SetOrientation(position.GetAbsoluteAngle(actor));
        TempSummon* dummy = actor->GetMap()->SummonCreature(entry, position, nullptr, 0, actor);
        if (!dummy)
            return nullptr;

        dummy->SetLevel(actor->GetLevel());
        dummy->SetFaction(_settings.dummyFaction);
        dummy->SetPhaseMask(actor->GetPhaseMask(), true);
        dummy->SetReactState(REACT_PASSIVE);
        // A stock AI evades (zeroing damage and healing to full) whenever its
        // threat list empties; suites need targets that just stand and bleed.
        dummy->AIM_Initialize(new NullCreatureAI(dummy));
        dummy->SetRegeneratingHealth(false);
        dummy->SetControlled(true, UNIT_STATE_ROOT);
        dummy->GetMotionMaster()->MoveIdle();
        dummy->SetMaxHealth(_settings.dummyHealth);
        dummy->SetHealth(_settings.dummyHealth);
        dummy->SetFacingToObject(actor);
        _dummies.push_back(dummy->GetGUID());
        return dummy;
    }

    bool DespawnDummy(ObjectGuid guid)
    {
        Creature* dummy = GetCreature(guid);
        if (!dummy)
            return false;

        dummy->CombatStop(true);
        dummy->DespawnOrUnsummon();
        std::erase(_dummies, guid);
        return true;
    }

    void DespawnAllDummies()
    {
        std::vector<ObjectGuid> dummies = std::move(_dummies);
        _dummies.clear();
        for (ObjectGuid guid : dummies)
            if (Creature* dummy = GetCreature(guid))
            {
                dummy->CombatStop(true);
                dummy->DespawnOrUnsummon();
            }

        if (Player* actor = GetActor())
            actor->CombatStop(true);
    }

    Unit* GetUnit(ObjectGuid guid) const
    {
        Player* actor = GetActor();
        if (!actor || !actor->IsInWorld())
            return nullptr;
        return ObjectAccessor::GetUnit(*actor, guid);
    }

    Creature* GetCreature(ObjectGuid guid) const
    {
        Player* actor = GetActor();
        if (!actor || !actor->IsInWorld())
            return nullptr;
        return ObjectAccessor::GetCreature(*actor, guid);
    }

    bool Engage(ObjectGuid guid, float threat)
    {
        Player* actor = GetActor();
        Creature* dummy = GetCreature(guid);
        if (!actor || !dummy || !dummy->IsAlive())
            return false;

        actor->SetInCombatWith(dummy);
        dummy->SetInCombatWith(actor);
        dummy->GetThreatMgr().AddThreat(actor, std::max(0.01f, threat), nullptr, true, true);
        return true;
    }

    bool Damage(ObjectGuid guid, uint32 amount)
    {
        Player* actor = GetActor();
        Creature* target = GetCreature(guid);
        if (!actor || !target || !target->IsAlive())
            return false;

        Unit::DealDamage(actor, target, amount, nullptr, DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL,
            nullptr, false, true);
        return true;
    }

    std::vector<Event> const& Events() const { return _events; }

    void ClearEvents()
    {
        _events.clear();
        _droppedEvents = 0;
    }

    void Observe(Event event)
    {
        if (!_transactionActive && !_run)
            return;

        if (!IsObserved(event.source) && !IsObserved(event.target))
            return;

        event.sequence = ++_eventSequence;
        event.timestampMs = GameTime::GetGameTimeMS().count();
        if (_events.size() >= _settings.maxEvents)
        {
            std::size_t trimmed = _events.size() - _settings.maxEvents + 1;
            _events.erase(_events.begin(), _events.begin() + trimmed);
            _droppedEvents += trimmed;
        }
        _events.push_back(std::move(event));
    }

    bool StartRun(std::string const& name, std::string& message)
    {
        if (_run || _pendingReport || _groupRun)
        {
            message = "a suite or suite group is already running";
            return false;
        }

        auto group = SuiteGroupRegistry().find(name);
        if (group == SuiteGroupRegistry().end())
            return StartSuite(name, message);

        if (group->second.empty())
        {
            message = "suite group is empty: " + name;
            return false;
        }
        for (std::string const& suite : group->second)
            if (SuiteRegistry().find(suite) == SuiteRegistry().end())
            {
                message = "suite group " + name + " contains unknown suite: " + suite;
                return false;
            }

        _groupRun = GroupRun{ name, group->second };
        bool started = StartNextGroupSuite(message);
        if (!started && !_pendingReport && !_run)
            _groupRun.reset();
        return started || _pendingReport.has_value() || _run.has_value();
    }

    bool StartNextGroupSuite(std::string& message)
    {
        if (!_groupRun || _groupRun->next >= _groupRun->suites.size())
            return false;

        std::string suite = _groupRun->suites[_groupRun->next++];
        return StartSuite(suite, message);
    }

    bool StartSuite(std::string const& suiteName, std::string& message)
    {
        if (_run || _pendingReport)
        {
            message = "a suite or its rollback verification is already running";
            return false;
        }

        auto factory = SuiteRegistry().find(suiteName);
        if (factory == SuiteRegistry().end())
        {
            message = "unknown suite: " + suiteName;
            return false;
        }

        if (!GetActor())
        {
            message = "actor is offline";
            return false;
        }

        Run run;
        run.name = suiteName;
        run.context._startedAtMs = GameTime::GetGameTimeMS().count();
        try
        {
            run.suite = factory->second();
            if (!run.suite)
                throw std::runtime_error("suite factory returned null");
        }
        catch (std::exception const& exception)
        {
            message = "suite factory failed: " + std::string(exception.what());
            return false;
        }
        catch (...)
        {
            message = "suite factory failed: unknown exception";
            return false;
        }

        run.transactional = run.suite->UsesTransaction();
        if (run.transactional && !BeginTransaction("suite:" + suiteName, message))
            return false;

        try
        {
            _run = std::move(run);
            _run->suite->Start(_run->context);
        }
        catch (std::exception const& exception)
        {
            PendingReport report;
            report.suite = suiteName;
            report.executionError = exception.what();
            std::string cleanupError = CancelSuiteAfterException();
            if (!cleanupError.empty())
                report.executionError += "; cleanup failed: " + cleanupError;
            bool transactional = _run && _run->transactional;
            report.transactional = transactional;
            if (_run)
            {
                report.assertions = std::move(_run->context._assertions);
                report.durationMs = _run->context.ElapsedMs();
            }
            _pendingReport = std::move(report);
            _run.reset();
            if (transactional)
            {
                if (!RollbackTransaction(true))
                    FinalizePendingReport(false, "transaction restore failed");
                message = "suite start failed; rollback queued";
            }
            else
            {
                FinalizePendingReport(true, {});
                message = "suite start failed";
            }
            return false;
        }
        catch (...)
        {
            PendingReport report;
            report.suite = suiteName;
            report.executionError = "unknown exception";
            std::string cleanupError = CancelSuiteAfterException();
            if (!cleanupError.empty())
                report.executionError += "; cleanup failed: " + cleanupError;
            bool transactional = _run && _run->transactional;
            report.transactional = transactional;
            if (_run)
            {
                report.assertions = std::move(_run->context._assertions);
                report.durationMs = _run->context.ElapsedMs();
            }
            _pendingReport = std::move(report);
            _run.reset();
            if (transactional)
            {
                if (!RollbackTransaction(true))
                    FinalizePendingReport(false, "transaction restore failed");
                message = "suite start failed; rollback queued";
            }
            else
            {
                FinalizePendingReport(true, {});
                message = "suite start failed";
            }
            return false;
        }

        message = "suite started: " + suiteName;
        LOG_INFO("module", "TestHarness: RUN suite={}", suiteName);
        return true;
    }

    std::string CancelSuiteAfterException()
    {
        if (!_run || !_run->suite)
            return {};

        try
        {
            _run->suite->Cancel(_run->context);
        }
        catch (std::exception const& exception)
        {
            return exception.what();
        }
        catch (...)
        {
            return "unknown exception";
        }
        return {};
    }

    void CancelRun(std::string reason)
    {
        if (!_run)
            return;
        _groupRun.reset();

        try
        {
            _run->suite->Cancel(_run->context);
        }
        catch (...)
        {
        }

        bool transactional = _run->transactional;
        PendingReport report;
        report.suite = _run->name;
        report.assertions = std::move(_run->context._assertions);
        report.durationMs = _run->context.ElapsedMs();
        report.executionError = std::move(reason);
        report.transactional = transactional;
        _pendingReport = std::move(report);
        _run.reset();
        if (!transactional)
            FinalizePendingReport(true, {});
    }

    std::string Status() const
    {
        std::ostringstream output;
        output << "enabled=" << _settings.enabled
               << " account=" << _settings.account << ':' << _accountId
               << " actor=" << _settings.actorName << ':' << _guid.GetCounter()
               << " state=";
        if (_creating) output << "provisioning";
        else if (_restore) output << "restoring";
        else if (_loading) output << "loading";
        else if (GetActor()) output << "online";
        else output << "offline";
        output << " transaction=" << (_transactionActive ? _transactionReason : "none")
               << " suite=" << (_run ? _run->name : (_pendingReport ? _pendingReport->suite + ":rollback" : "none"))
               << " dummies=" << _dummies.size()
               << " events=" << _events.size()
               << " dropped_events=" << _droppedEvents;
        if (!_disabledReason.empty())
            output << " error=\"" << _disabledReason << '"';
        return output.str();
    }

    void PrintEvents(ChatHandler* handler, uint64 since) const
    {
        for (Event const& event : _events)
        {
            if (event.sequence <= since)
                continue;
            std::ostringstream line;
            line << "[TestHarnessEvent] seq=" << event.sequence
                 << " ms=" << event.timestampMs
                 << " type=" << EventTypeName(event.type)
                 << " source=" << GuidText(event.source)
                 << " target=" << GuidText(event.target)
                 << " spell=" << event.spellId
                 << " amount=" << event.amount
                 << " result=" << event.result;
            handler->SendSysMessage(line.str());
        }

        if (_droppedEvents)
            handler->SendSysMessage("[TestHarnessEvent] dropped=" + std::to_string(_droppedEvents));
    }

    uint32 DefaultDummyEntry() const { return _settings.dummyEntry; }

private:
    struct Run
    {
        std::string name;
        std::unique_ptr<Suite> suite;
        Context context;
        bool transactional = true;
    };

    enum class RestoreKind
    {
        Baseline,
        Transaction
    };

    enum class RestoreStage
    {
        WaitingForReplace,
        WaitingForImport
    };

    struct Restore
    {
        std::filesystem::path path;
        uint32 accountId = 0;
        ObjectGuid::LowType guid;
        std::string name;
        bool restart = false;
        RestoreKind kind = RestoreKind::Baseline;
        RestoreStage stage = RestoreStage::WaitingForReplace;
        uint64 startedAtMs = 0;
        uint32 pollElapsed = 0;
    };

    Manager() = default;

    void RefreshPaths()
    {
        _baselinePath = _settings.stateDirectory / "actor-baseline.sql";
        _transactionPath = _settings.stateDirectory / "actor-transaction.sql";
        _markerPath = _settings.stateDirectory / "actor-transaction.active";
    }

    void Disable(std::string reason)
    {
        RequestDisable(std::move(reason), true);
    }

    void RequestDisable(std::string reason, bool error)
    {
        if (!_disablePending)
        {
            _disablePending = true;
            _disabledReason = std::move(reason);
            if (error)
                LOG_ERROR("module", "TestHarness: disabling: {}", _disabledReason);
            else
                LOG_INFO("module", "TestHarness: disabling: {}", _disabledReason);
        }

        CancelRun(_disabledReason);
        DespawnAllDummies();
        _waitingForAccount = false;
        _creating = false;
        StopCreationSession();

        if (_restore)
        {
            if (_pendingReport)
                _restore->restart = true;
        }
        else if (_transactionActive || std::filesystem::exists(_markerPath))
        {
            if (!RollbackTransaction(true))
            {
                FinalizePendingReport(false, "transaction rollback could not be queued while disabling");
                StopActorInternal();
            }
        }
        else
            StopActorInternal();

        FinishDisableIfReady();
    }

    void FinishDisableIfReady()
    {
        if (!_disablePending || _restore || _loading || _initializing || _creating
            || _transactionActive || _run || _pendingReport)
            return;

        DespawnAllDummies();
        StopActorInternal();
        StopCreationSession();
        _verifyFingerprintAfterLogin = false;
        _settings.enabled = false;
        _disablePending = false;
        LOG_INFO("module", "TestHarness: disabled");
    }

    void ContinueStartupAfterAccount()
    {
        ObjectGuid existing = sCharacterCache->GetCharacterGuidByName(_settings.actorName);
        if (existing && sCharacterCache->GetCharacterAccountIdByGuid(existing) != _accountId)
        {
            Disable("actor name belongs to a different account: " + _settings.actorName);
            return;
        }

        _guid = existing;
        if (!_guid && std::filesystem::exists(_markerPath))
        {
            uint32 markerAccount = 0;
            ObjectGuid::LowType markerGuid;
            std::string markerName;
            if (!ReadMarker(markerAccount, markerGuid, markerName)
                || markerAccount != _accountId || markerName != _settings.actorName)
            {
                Disable("interrupted transaction marker does not belong to the configured actor");
                return;
            }
            _guid = ObjectGuid::Create<HighGuid::Player>(markerGuid);
        }

        if (!_guid)
        {
            if (std::filesystem::exists(_baselinePath))
            {
                if (!QueueRestore(_baselinePath, _accountId, ObjectGuid::LowType(),
                    _settings.actorName, _settings.autoStart, RestoreKind::Baseline))
                    Disable("could not queue missing actor recovery from baseline");
                return;
            }

            BeginCharacterCreation();
            return;
        }

        CompleteStartup();
    }

    void BeginCharacterCreation()
    {
        _creationSession = std::make_unique<WorldSession>(
            _accountId, std::string(_settings.account), 0, nullptr, SEC_PLAYER,
            EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), sWorld->GetDefaultDbcLocale(),
            0, false, true, 0, true);

        WorldPacket packet(CMSG_CHAR_CREATE, 32);
        packet << _settings.actorName
               << _settings.race
               << _settings.playerClass
               << _settings.gender
               << uint8(0) << uint8(0) << uint8(0) << uint8(0) << uint8(0) << uint8(0);
        _creating = true;
        _provisionElapsed = 0;
        _creationSession->HandleCharCreateOpcode(packet);
        LOG_INFO("module", "TestHarness: provisioning dedicated actor {}", _settings.actorName);
    }

    void StopCreationSession()
    {
        _creationSession.reset();
    }

    void CompleteStartup()
    {
        if (!_guid)
        {
            Disable("actor GUID is unavailable after provisioning");
            return;
        }

        if (std::filesystem::exists(_markerPath))
        {
            _transactionActive = true;
            _transactionReason = "startup-recovery";
            if (!RollbackTransaction(_settings.autoStart))
                Disable("interrupted transaction recovery could not be queued");
            return;
        }

        if (std::filesystem::exists(_baselinePath))
        {
            if (!QueueRestore(_baselinePath, _accountId, _guid.GetCounter(), _settings.actorName,
                _settings.autoStart, RestoreKind::Baseline))
                Disable("startup baseline restore could not be queued");
            return;
        }

        std::string message;
        if (!StartActor(message))
            Disable("actor autostart failed: " + message);
    }

    void FinishActorLogin(LoginQueryHolder const& holder)
    {
        if (!_loading)
            return;

        _loading = false;
        _session = std::make_unique<WorldSession>(
            _accountId, std::string(_settings.account), 0, nullptr, SEC_PLAYER,
            EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), sWorld->GetDefaultDbcLocale(),
            0, false, true, 0, true);
        _session->HandlePlayerLoginFromDB(holder);

        Player* actor = GetActor();
        if (!actor)
        {
            _session.reset();
            Disable("actor login query completed without a player");
            FinalizePendingReport(false, "actor failed to reload after rollback");
            return;
        }

        if (!std::filesystem::exists(_baselinePath))
        {
            InitializeActor(*actor);
            return;
        }

        AcknowledgeTeleport(actor);
        OnActorReady();
    }

    void InitializeActor(Player& actor)
    {
        _initializing = true;
        _initializationElapsed = 0;
        _initializationRetryElapsed = 0;
        actor.CombatStop(true);
        actor.Dismount();

        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = actor.GetBagByPos(bagSlot))
                for (int32 slot = int32(bag->GetBagSize()) - 1; slot >= 0; --slot)
                    if (bag->GetItemByPos(uint32(slot)))
                        actor.DestroyItem(bagSlot, uint8(slot), true);

        for (int32 slot = INVENTORY_SLOT_ITEM_END - 1; slot >= EQUIPMENT_SLOT_START; --slot)
            if (actor.GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(slot)))
                actor.DestroyItem(INVENTORY_SLOT_BAG_0, uint8(slot), true);

        actor.SetGameMaster(true);
        actor.GetAchievementMgr()->Reset();
        actor.GiveLevel(std::min<uint8>(_settings.level, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)));
        actor.SetGameMaster(false);
        DeleteAchievementRows(actor.GetGUID().GetCounter());
        actor.InitTalentForLevel();
        actor.SetUInt32Value(PLAYER_XP, 0);
        for (uint32 spell : _settings.spells)
            if (sSpellMgr->GetSpellInfo(spell))
                actor.learnSpell(spell, false);

        actor.SetPhaseMask(_settings.phaseMask, true);
        actor.RemoveAtLoginFlag(AT_LOGIN_FIRST);
        if (!actor.TeleportTo(_settings.map, _settings.x, _settings.y, _settings.z, _settings.o))
            LOG_DEBUG("module", "TestHarness: initial actor teleport deferred until login settles");
        AcknowledgeTeleport(&actor);
    }

    void FinishActorInitialization(uint32 diff)
    {
        Player* actor = GetActor();
        _initializationElapsed += diff;
        _initializationRetryElapsed += diff;
        if (!actor)
            return;

        if (_initializationElapsed >= 10000)
        {
            LOG_ERROR("module",
                "TestHarness: baseline teleport timeout map={} position={:.3f},{:.3f},{:.3f} in_world={} far={} near={} destination_map={} target_map={}",
                actor->GetMapId(), actor->GetPositionX(), actor->GetPositionY(), actor->GetPositionZ(),
                actor->IsInWorld(), actor->IsBeingTeleportedFar(), actor->IsBeingTeleportedNear(),
                actor->GetTeleportDest().GetMapId(), _settings.map);
            _initializing = false;
            Disable("actor could not reach the configured baseline location");
            return;
        }

        float dx = actor->GetPositionX() - _settings.x;
        float dy = actor->GetPositionY() - _settings.y;
        float dz = actor->GetPositionZ() - _settings.z;
        bool atBaseline = actor->GetMapId() == _settings.map && dx * dx + dy * dy + dz * dz <= 4.0f;
        if (!atBaseline)
        {
            if (!actor->IsBeingTeleported() && actor->IsInWorld() && _initializationRetryElapsed >= 250)
            {
                _initializationRetryElapsed = 0;
                if (actor->TeleportTo(_settings.map, _settings.x, _settings.y, _settings.z, _settings.o))
                    AcknowledgeTeleport(actor);
            }
            return;
        }

        if (actor->IsBeingTeleported() || !actor->IsInWorld())
            return;

        actor->SetPhaseMask(_settings.phaseMask, true);
        actor->SetFullHealth();
        actor->SetPower(actor->getPowerType(), actor->GetMaxPower(actor->getPowerType()));
        actor->RemoveAllSpellCooldown();
        SaveActorSynchronously(*actor);
        if (!WriteDump(_baselinePath, _guid.GetCounter()))
        {
            _initializing = false;
            Disable("could not write initial actor baseline");
            return;
        }

        _initializing = false;
        LOG_INFO("module", "TestHarness: actor baseline ready name={} guid={} level={} map={} position={:.3f},{:.3f},{:.3f}",
            actor->GetName(), actor->GetGUID().GetCounter(), actor->GetLevel(), actor->GetMapId(),
            actor->GetPositionX(), actor->GetPositionY(), actor->GetPositionZ());
        if (_settings.autoStart)
            OnActorReady();
        else
            StopActorInternal();
    }

    void OnActorReady()
    {
        Player* actor = GetActor();
        if (!actor)
            return;

        actor->CombatStop(true);
        actor->Dismount();
        if (actor->GetMotionMaster())
        {
            actor->GetMotionMaster()->Clear(true);
            actor->StopMoving();
        }

        if (_verifyFingerprintAfterLogin)
        {
            _verifyFingerprintAfterLogin = false;
            Fingerprint actual = FingerprintActor(*actor);
            bool exact = actual == _transactionFingerprint;
            if (exact)
                LOG_INFO("module", "TestHarness: rollback fingerprint PASS value={} summary={}", actual.value, actual.summary);
            else
                LOG_ERROR("module", "TestHarness: rollback fingerprint FAIL expected={} ({}) actual={} ({})",
                    _transactionFingerprint.value, _transactionFingerprint.summary, actual.value, actual.summary);
            FinalizePendingReport(exact, exact ? std::string() : "actor fingerprint differs after rollback");
        }

        LOG_INFO("module", "TestHarness: actor online name={} guid={} map={}",
            actor->GetName(), actor->GetGUID().GetCounter(), actor->GetMapId());
    }

    static void AcknowledgeTeleport(Player* actor)
    {
        if (!actor || !actor->GetSession())
            return;

        if (actor->IsBeingTeleportedFar() && !actor->IsInWorld())
        {
            actor->GetSession()->HandleMoveWorldportAck();
            return;
        }

        if (actor->IsBeingTeleportedNear() && actor->IsInWorld())
        {
            Player* mover = actor->m_mover ? actor->m_mover->ToPlayer() : nullptr;
            if (!mover)
                return;

            WorldPacket packet(MSG_MOVE_TELEPORT_ACK, 20);
            packet << mover->GetPackGUID() << uint32(0) << uint32(0);
            actor->GetSession()->HandleMoveTeleportAck(packet);
        }
    }

    void StopActorInternal()
    {
        _loading = false;
        _initializing = false;
        if (!_session)
            return;

        if (_session->GetPlayer())
            _session->LogoutPlayer(false);
        _session.reset();
    }

    static void SaveActorSynchronously(Player& actor)
    {
        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        actor.SaveToDB(transaction, false, false);

        bool done = false;
        TransactionCallback callback = CharacterDatabase.AsyncCommitTransaction(transaction);
        callback.AfterComplete([&done](bool) { done = true; });
        while (!done)
        {
            if (!callback.InvokeIfReady())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool WriteDump(std::filesystem::path const& path, ObjectGuid::LowType guid)
    {
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);

        std::string dump;
        DumpReturn result = PlayerDumpWriter().WriteDumpToString(dump, guid);
        if (result != DUMP_SUCCESS)
        {
            LOG_ERROR("module", "TestHarness: dump generation failed path={} result={}", temporary.string(), result);
            return false;
        }

        for (auto const& [name, participant] : SnapshotParticipantRegistry())
        {
            std::string payload;
            if (!participant.capture(guid, payload) || payload.find_first_of("\r\n") != std::string::npos)
            {
                LOG_ERROR("module", "TestHarness: snapshot participant capture failed: {}", name);
                return false;
            }
            dump += std::string(SNAPSHOT_PREFIX) + name + ' ' + payload + '\n';
        }

        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(dump.data(), std::streamsize(dump.size()));
        output.flush();
        if (!output)
        {
            LOG_ERROR("module", "TestHarness: dump file write failed path={}", temporary.string());
            return false;
        }
        output.close();

        std::filesystem::rename(temporary, path, ignored);
        if (ignored)
        {
            LOG_ERROR("module", "TestHarness: dump rename failed {} -> {}: {}",
                temporary.string(), path.string(), ignored.message());
            return false;
        }
        return true;
    }

    using SnapshotRestorePlans = std::vector<std::pair<std::string, SnapshotRestorePlan>>;

    static std::optional<SnapshotRestorePlans> PrepareSnapshotParticipants(
        std::filesystem::path const& path, ObjectGuid::LowType guid)
    {
        SnapshotRestorePlans plans;
        if (SnapshotParticipantRegistry().empty())
            return plans;

        std::ifstream input(path);
        if (!input)
            return std::nullopt;

        std::map<std::string, std::string, std::less<>> payloads;
        std::string line;
        while (std::getline(input, line))
        {
            if (!line.starts_with(SNAPSHOT_PREFIX))
                continue;

            std::string_view record(line);
            record.remove_prefix(SNAPSHOT_PREFIX.size());
            std::size_t separator = record.find(' ');
            if (separator == std::string_view::npos)
                return std::nullopt;
            payloads.emplace(record.substr(0, separator), record.substr(separator + 1));
        }

        for (auto const& [name, participant] : SnapshotParticipantRegistry())
        {
            auto payload = payloads.find(name);
            if (payload == payloads.end())
                continue;

            std::optional<SnapshotRestorePlan> plan = participant.restore(guid, payload->second);
            if (!plan)
            {
                LOG_ERROR("module", "TestHarness: snapshot participant prepare failed: {}", name);
                return std::nullopt;
            }
            plans.emplace_back(name, std::move(*plan));
        }
        return plans;
    }

    static bool AppendSnapshotParticipants(
        SnapshotRestorePlans& plans, CharacterDatabaseTransaction const& transaction)
    {
        for (auto& [name, plan] : plans)
        {
            if (plan.append && !plan.append(transaction))
            {
                LOG_ERROR("module", "TestHarness: snapshot participant append failed: {}", name);
                return false;
            }
        }
        return true;
    }

    static void CommitSnapshotParticipants(SnapshotRestorePlans& plans)
    {
        for (auto& [name, plan] : plans)
        {
            (void)name;
            if (plan.commit)
                plan.commit();
        }
    }

    static bool CharacterExists(ObjectGuid::LowType guid)
    {
        if (!guid)
            return false;

        CharacterDatabasePreparedStatement* statement = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_GUID);
        statement->SetData(0, guid);
        return bool(CharacterDatabase.Query(statement));
    }

    bool QueueRestore(std::filesystem::path const& path, uint32 accountId, ObjectGuid::LowType guid,
        std::string const& name, bool restart, RestoreKind kind)
    {
        if (_restore)
            return false;

        if (!std::filesystem::exists(path))
        {
            LOG_ERROR("module", "TestHarness: restore dump does not exist: {}", path.string());
            return false;
        }

        DespawnAllDummies();
        StopActorInternal();
        ObjectGuid fullGuid = ObjectGuid::Create<HighGuid::Player>(guid);
        if (guid && ObjectAccessor::FindConnectedPlayer(fullGuid))
        {
            LOG_ERROR("module", "TestHarness: refusing to replace actor while another session owns it");
            return false;
        }


        Restore restore;
        restore.path = path;
        restore.accountId = accountId;
        restore.guid = guid;
        restore.name = name;
        restore.restart = restart;
        restore.kind = kind;
        restore.startedAtMs = GameTime::GetGameTimeMS().count();
        _restore = std::move(restore);
        LOG_INFO("module", "TestHarness: restore queued path={} actor={} guid={}",
            path.string(), name, guid);
        return true;
    }

    void FailRestore(std::string reason)
    {
        LOG_ERROR("module", "TestHarness: restore failed: {}", reason);
        _restore.reset();
        _verifyFingerprintAfterLogin = false;
        if (_pendingReport)
            FinalizePendingReport(false, reason);
    }

    static void DeleteAchievementRows(ObjectGuid::LowType guid)
    {
        if (!guid)
            return;

        CharacterDatabase.DirectExecute("DELETE FROM character_achievement WHERE guid = {}", guid);
        CharacterDatabase.DirectExecute("DELETE FROM character_achievement_progress WHERE guid = {}", guid);
    }

    void UpdateRestore(uint32 diff)
    {
        Restore& restore = *_restore;
        restore.pollElapsed += diff;
        if (restore.pollElapsed < 100)
            return;
        restore.pollElapsed = 0;

        if (restore.stage == RestoreStage::WaitingForReplace)
        {


            if (restore.guid)
            {
                ObjectGuidGeneratorBase& generator =
                    sObjectMgr->GetGenerator<HighGuid::Player>();
                if (generator.GetNextAfterMaxUsed() <= restore.guid)
                    generator.Set(restore.guid + 1);
            }


            std::optional<SnapshotRestorePlans> participantPlans =
                PrepareSnapshotParticipants(restore.path, restore.guid);
            if (!participantPlans)
            {
                FailRestore("snapshot participant prepare failed");
                return;
            }
            DumpReturn result = PlayerDumpReader().LoadDumpFromFile(
                restore.path.string(), restore.accountId, restore.name, restore.guid, true, true, true,
                [&](CharacterDatabaseTransaction const& transaction)
                {
                    return AppendSnapshotParticipants(*participantPlans, transaction);
                });
            if (result != DUMP_SUCCESS)
            {
                FailRestore("dump import returned " + std::to_string(result));
                return;
            }
            CommitSnapshotParticipants(*participantPlans);

            ObjectGuid importedGuid = sCharacterCache->GetCharacterGuidByName(restore.name);
            if (!importedGuid)
            {
                FailRestore("dump import did not create the requested cache entry");
                return;
            }
            restore.guid = importedGuid.GetCounter();
            restore.stage = RestoreStage::WaitingForImport;
            return;
        }

        if (!CharacterExists(restore.guid))
        {
            if (_settings.transactionTimeoutSeconds
                && uint64(GameTime::GetGameTimeMS().count()) - restore.startedAtMs
                    >= uint64(_settings.transactionTimeoutSeconds) * IN_MILLISECONDS)
                FailRestore("database restore timed out waiting for dump import");
            return;
        }

        ObjectGuid fullGuid = ObjectGuid::Create<HighGuid::Player>(restore.guid);
        std::optional<CharacterCacheEntry> preservedCache;
        if (CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(fullGuid))
            preservedCache = *cache;

        sCharacterCache->RefreshCacheEntry(restore.guid);
        if (preservedCache)
        {
            sCharacterCache->UpdateCharacterGuildId(fullGuid, preservedCache->GuildId);
            for (uint8 slot = 0; slot < MAX_ARENA_SLOT; ++slot)
                sCharacterCache->UpdateCharacterArenaTeamId(fullGuid, slot, preservedCache->ArenaTeamId[slot]);
            sCharacterCache->UpdateCharacterGroup(fullGuid, preservedCache->GroupGuid);
        }

        sMailMgr->RecountMailCount(restore.guid);
        ObjectGuid importedGuid = sCharacterCache->GetCharacterGuidByName(restore.name);
        if (!importedGuid || importedGuid.GetCounter() != restore.guid
            || sCharacterCache->GetCharacterAccountIdByGuid(importedGuid) != restore.accountId)
        {
            FailRestore("committed actor does not match the requested GUID, name, and account");
            return;
        }

        Restore completed = std::move(restore);
        _restore.reset();
        _accountId = completed.accountId;
        _guid = importedGuid;

        if (completed.kind == RestoreKind::Transaction)
        {
            DiscardTransactionArtifacts();
            _verifyFingerprintAfterLogin = true;
            LOG_INFO("module", "TestHarness: transaction restored actor={} expected_fingerprint={}",
                completed.name, _transactionFingerprint.value);
        }
        else
            LOG_INFO("module", "TestHarness: baseline restored actor={} guid={}", completed.name, completed.guid);

        if (!completed.restart)
            return;

        std::string message;
        if (!StartActor(message))
        {
            _verifyFingerprintAfterLogin = false;
            if (_pendingReport)
                FinalizePendingReport(false, "restored actor could not restart: " + message);
            LOG_ERROR("module", "TestHarness: restored actor could not restart: {}", message);
        }
    }

    bool ReadMarker(uint32& accountId, ObjectGuid::LowType& guid, std::string& name)
    {
        std::ifstream marker(_markerPath);
        uint64 parsedGuid = 0;
        uint64 fingerprint = 0;
        std::string summary;
        if (!(marker >> accountId >> parsedGuid >> name >> fingerprint >> summary))
            return false;

        std::string ignored;
        std::getline(marker, ignored);
        std::getline(marker, _transactionReason);
        guid = ObjectGuid::LowType(parsedGuid);
        _transactionFingerprint = { fingerprint, std::move(summary) };
        return true;
    }

    void DiscardTransactionArtifacts()
    {
        std::error_code ignored;
        std::filesystem::remove(_markerPath, ignored);
        std::filesystem::remove(_markerPath.string() + ".tmp", ignored);
        std::filesystem::remove(_transactionPath, ignored);
        std::filesystem::remove(_transactionPath.string() + ".tmp", ignored);
        _transactionActive = false;
        _transactionReason.clear();
    }

    Fingerprint FingerprintActor(Player const& actor) const
    {
        Fingerprint fingerprint;
        auto add = [&](uint64 value) { HashValue(fingerprint.value, value); };
        add(actor.GetLevel());
        add(actor.GetHealth());
        add(actor.GetMaxHealth());
        add(actor.GetMoney());
        add(actor.GetMapId());
        add(actor.GetPhaseMask());
        add(uint64(std::llround(actor.GetPositionX() * 1000.0f)));
        add(uint64(std::llround(actor.GetPositionY() * 1000.0f)));
        add(uint64(std::llround(actor.GetPositionZ() * 1000.0f)));
        add(uint64(std::llround(actor.GetOrientation() * 100000.0f)));
        for (uint8 power = POWER_MANA; power < MAX_POWERS; ++power)
        {
            add(actor.GetPower(Powers(power)));
            add(actor.GetMaxPower(Powers(power)));
        }

        std::size_t spellCount = 0;
        for (auto const& [spellId, spell] : actor.GetSpellMap())
        {
            if (!spell || spell->State == PLAYERSPELL_REMOVED)
                continue;
            add(spellId);
            add(spell->Active);
            add(spell->specMask);
            ++spellCount;
        }

        std::size_t cooldownCount = 0;
        for (auto const& [spellId, cooldown] : actor.GetSpellCooldownMap())
        {
            add(spellId);
            add(cooldown.category);
            add(cooldown.itemid);
            ++cooldownCount;
        }

        std::size_t auraCount = 0;
        for (auto const& [spellId, application] : actor.GetAppliedAuras())
        {
            Aura const* aura = application ? application->GetBase() : nullptr;
            if (!aura)
                continue;
            add(spellId);
            add(aura->GetStackAmount());
            add(uint32(aura->GetMaxDuration()));
            for (uint8 index = EFFECT_0; index < MAX_SPELL_EFFECTS; ++index)
                if (AuraEffect const* effect = aura->GetEffect(index))
                {
                    add(index);
                    add(uint32(effect->GetAmount()));
                }
            ++auraCount;
        }

        std::vector<uint32> skillIds;
        skillIds.reserve(actor.GetSkillStatusMap().size());
        for (auto const& [skillId, status] : actor.GetSkillStatusMap())
            if (status.uState != SKILL_DELETED)
                skillIds.push_back(skillId);
        std::sort(skillIds.begin(), skillIds.end());
        for (uint32 skillId : skillIds)
        {
            add(skillId);
            add(actor.GetSkillStep(skillId));
            add(actor.GetPureSkillValue(skillId));
            add(actor.GetPureMaxSkillValue(skillId));
        }

        std::size_t reputationCount = 0;
        for (auto const& [listId, state] : actor.GetReputationMgr().GetStateList())
        {
            add(listId);
            add(state.ID);
            add(uint32(state.Standing));
            add(state.Flags);
            ++reputationCount;
        }

        std::size_t mailCount = 0;
        for (Mail const* mail : actor.GetMails())
        {
            if (!mail || mail->state == MAIL_STATE_DELETED)
                continue;
            add(mail->messageType);
            add(mail->stationery);
            add(mail->mailTemplateId);
            add(mail->sender);
            add(std::hash<std::string>{}(mail->subject));
            add(std::hash<std::string>{}(mail->body));
            add(mail->money);
            add(mail->COD);
            add(mail->checked);
            for (MailItemInfo const& item : mail->items)
                add(item.item_template);
            ++mailCount;
        }

        std::size_t itemCount = 0;
        auto addItem = [&](uint16 position, Item const* item)
        {
            if (!item)
                return;
            add(position);
            add(item->GetEntry());
            add(item->GetCount());
            add(uint32(item->GetItemRandomPropertyId()));
            add(item->GetItemPropertySeed());
            add(item->GetUInt32Value(ITEM_FIELD_DURABILITY));
            ++itemCount;
        };

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < PLAYER_SLOT_END; ++slot)
        {
            Item const* item = actor.GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            addItem(slot, item);
            if (Bag const* bag = item ? item->ToBag() : nullptr)
                for (uint32 bagSlot = 0; bagSlot < bag->GetBagSize(); ++bagSlot)
                    addItem(uint16(slot) << 8 | uint16(bagSlot), bag->GetItemByPos(bagSlot));
        }

        std::ostringstream summary;
        summary << "level=" << uint32(actor.GetLevel())
                << ",health=" << actor.GetHealth() << '/' << actor.GetMaxHealth()
                << ",money=" << actor.GetMoney()
                << ",map=" << actor.GetMapId()
                << ",phase=" << actor.GetPhaseMask()
                << ",spells=" << spellCount
                << ",cooldowns=" << cooldownCount
                << ",auras=" << auraCount
                << ",skills=" << skillIds.size()
                << ",reputations=" << reputationCount
                << ",mails=" << mailCount
                << ",items=" << itemCount;
        fingerprint.summary = summary.str();
        return fingerprint;
    }

    bool IsObserved(ObjectGuid guid) const
    {
        if (!guid)
            return false;
        if (guid == _guid)
            return true;
        return std::find(_dummies.begin(), _dummies.end(), guid) != _dummies.end();
    }

    void UpdateRun(uint32 diff)
    {
        if (!_run)
            return;

        std::string executionError;
        try
        {
            if (!_run->context.IsFinished())
                _run->suite->Update(_run->context, diff);
        }
        catch (std::exception const& exception)
        {
            executionError = exception.what();
            _run->context.Fail("unhandled exception", executionError);
            std::string cleanupError = CancelSuiteAfterException();
            if (!cleanupError.empty())
                executionError += "; cleanup failed: " + cleanupError;
        }
        catch (...)
        {
            executionError = "unknown exception";
            _run->context.Fail("unhandled exception", executionError);
            std::string cleanupError = CancelSuiteAfterException();
            if (!cleanupError.empty())
                executionError += "; cleanup failed: " + cleanupError;
        }

        if (!_run->context.IsFinished())
            return;

        PendingReport report;
        report.suite = _run->name;
        report.assertions = std::move(_run->context._assertions);
        report.durationMs = _run->context.ElapsedMs();
        report.executionPassed = std::all_of(report.assertions.begin(), report.assertions.end(),
            [](Assertion const& assertion) { return assertion.passed; });
        report.executionError = std::move(executionError);
        bool transactional = _run->transactional;
        report.transactional = transactional;
        _pendingReport = std::move(report);
        _run.reset();
        if (transactional)
        {
            if (!RollbackTransaction(true))
                FinalizePendingReport(false, "transaction restore failed");
        }
        else
            FinalizePendingReport(true, {});
    }

    void FinalizePendingReport(bool rollbackPassed, std::string rollbackError)
    {
        if (!_pendingReport)
            return;

        PendingReport report = std::move(*_pendingReport);
        _pendingReport.reset();
        std::size_t passed = std::count_if(report.assertions.begin(), report.assertions.end(),
            [](Assertion const& assertion) { return assertion.passed; });
        std::size_t failed = report.assertions.size() - passed;
        bool success = report.executionPassed && report.executionError.empty() && rollbackPassed;

        for (Assertion const& assertion : report.assertions)
            LOG_INFO("module", "[TestHarnessAssertion] suite={} result={} name=\"{}\" detail=\"{}\"",
                report.suite, assertion.passed ? "PASS" : "FAIL", assertion.name, assertion.detail);

        if (success)
            LOG_INFO("module", "[TestHarness] PASS suite={} passed={} failed={} duration_ms={} rollback={}",
                report.suite, passed, failed, report.durationMs,
                report.transactional ? "exact" : "none");
        else
            LOG_ERROR("module", "[TestHarness] FAIL suite={} passed={} failed={} duration_ms={} execution_error=\"{}\" rollback_error=\"{}\"",
                report.suite, passed, failed, report.durationMs, report.executionError, rollbackError);
        if (_groupRun)
        {
            _groupRun->durationMs += report.durationMs;
            success ? ++_groupRun->passed : ++_groupRun->failed;
            if (_groupRun->next < _groupRun->suites.size())
            {
                std::string message;
                bool started = StartNextGroupSuite(message);
                if (started || _pendingReport || _run || !_groupRun)
                    return;

                ++_groupRun->failed;
                LOG_ERROR("module", "[TestHarnessGroup] FAIL group={} passed={} failed={} duration_ms={} start_error=\"{}\"",
                    _groupRun->name, _groupRun->passed, _groupRun->failed,
                    _groupRun->durationMs, message);
                _groupRun.reset();
                return;
            }

            if (_groupRun->failed == 0)
                LOG_INFO("module", "[TestHarnessGroup] PASS group={} passed={} failed=0 duration_ms={}",
                    _groupRun->name, _groupRun->passed, _groupRun->durationMs);
            else
                LOG_ERROR("module", "[TestHarnessGroup] FAIL group={} passed={} failed={} duration_ms={}",
                    _groupRun->name, _groupRun->passed, _groupRun->failed, _groupRun->durationMs);
            _groupRun.reset();
        }

    }

    Settings _settings;
    uint32 _accountId = 0;
    ObjectGuid _guid;
    std::unique_ptr<WorldSession> _creationSession;
    std::unique_ptr<WorldSession> _session;
    bool _creating = false;
    bool _loading = false;
    bool _initializing = false;
    uint32 _initializationElapsed = 0;
    uint32 _initializationRetryElapsed = 0;
    uint32 _provisionElapsed = 0;
    bool _waitingForAccount = false;
    uint32 _accountPollElapsed = 0;
    std::string _disabledReason;
    bool _disablePending = false;

    std::filesystem::path _baselinePath;
    std::filesystem::path _transactionPath;
    std::filesystem::path _markerPath;
    bool _transactionActive = false;
    uint64 _transactionStartedAt = 0;
    std::string _transactionReason;
    Fingerprint _transactionFingerprint;
    bool _verifyFingerprintAfterLogin = false;

    std::vector<ObjectGuid> _dummies;
    std::vector<Event> _events;
    uint64 _eventSequence = 0;
    uint64 _droppedEvents = 0;
    std::optional<Run> _run;
    std::optional<PendingReport> _pendingReport;
    std::optional<GroupRun> _groupRun;
    std::optional<Restore> _restore;
};

Context::Context() = default;

Player* Context::GetActor() const { return Manager::Instance().GetActor(); }
Unit* Context::GetUnit(ObjectGuid guid) const { return Manager::Instance().GetUnit(guid); }
Creature* Context::GetCreature(ObjectGuid guid) const { return Manager::Instance().GetCreature(guid); }
Creature* Context::SpawnDummy(float distance, float angleOffset, uint32 entry)
{
    return Manager::Instance().SpawnDummy(distance, angleOffset, entry);
}
bool Context::DespawnDummy(ObjectGuid guid) { return Manager::Instance().DespawnDummy(guid); }
void Context::DespawnAllDummies() { Manager::Instance().DespawnAllDummies(); }
bool Context::Engage(ObjectGuid guid, float threat) { return Manager::Instance().Engage(guid, threat); }
bool Context::Damage(ObjectGuid guid, uint32 amount) { return Manager::Instance().Damage(guid, amount); }
std::vector<Event> const& Context::GetEvents() const { return Manager::Instance().Events(); }
Event const* Context::FindEvent(EventType type, ObjectGuid source, ObjectGuid target, uint32 spellId) const
{
    auto const& events = GetEvents();
    auto const found = std::find_if(events.rbegin(), events.rend(), [&](Event const& event)
    {
        return event.type == type
            && (source.IsEmpty() || event.source == source)
            && (target.IsEmpty() || event.target == target)
            && (!spellId || event.spellId == spellId);
    });
    return found != events.rend() ? &*found : nullptr;
}

std::size_t Context::CountEvents(EventType type, ObjectGuid source, ObjectGuid target, uint32 spellId) const
{
    auto const& events = GetEvents();
    return std::count_if(events.begin(), events.end(), [&](Event const& event)
    {
        return event.type == type
            && (source.IsEmpty() || event.source == source)
            && (target.IsEmpty() || event.target == target)
            && (!spellId || event.spellId == spellId);
    });
}

void Context::ClearEvents() { Manager::Instance().ClearEvents(); }

void Context::Expect(bool condition, std::string name, std::string detail)
{
    _assertions.push_back({ condition, std::move(name), std::move(detail) });
}

void Context::Finish() { _finished = true; }

void Context::Fail(std::string name, std::string detail)
{
    Expect(false, std::move(name), std::move(detail));
    Finish();
}

bool Context::IsFinished() const { return _finished; }
uint32 Context::ElapsedMs() const
{
    uint64 elapsed = uint64(GameTime::GetGameTimeMS().count()) - _startedAtMs;
    return uint32(std::min<uint64>(elapsed, std::numeric_limits<uint32>::max()));
}

bool RegisterSuite(std::string name, SuiteFactory factory)
{
    if (name.empty() || !factory)
        return false;
    return SuiteRegistry().emplace(std::move(name), std::move(factory)).second;
}

bool UnregisterSuite(std::string_view name)
{
    auto found = SuiteRegistry().find(name);
    if (found == SuiteRegistry().end())
        return false;
    SuiteRegistry().erase(found);
    return true;
}

std::vector<std::string> RegisteredSuites()
{
    std::vector<std::string> names;
    names.reserve(SuiteRegistry().size());
    for (auto const& [name, factory] : SuiteRegistry())
    {
        (void)factory;
        names.push_back(name);
    }
    return names;
}
bool RegisterSuiteGroup(std::string name, std::vector<std::string> suites)
{
    if (name.empty() || suites.empty()
        || std::any_of(suites.begin(), suites.end(), [](std::string const& suite) { return suite.empty(); }))
        return false;
    return SuiteGroupRegistry().emplace(std::move(name), std::move(suites)).second;
}

std::vector<std::string> RegisteredSuiteGroups()
{
    std::vector<std::string> names;
    names.reserve(SuiteGroupRegistry().size());
    for (auto const& [name, suites] : SuiteGroupRegistry())
    {
        (void)suites;
        names.push_back(name);
    }
    return names;
}

bool RegisterSnapshotParticipant(std::string name, SnapshotCapture capture, SnapshotRestore restore)
{
    if (name.empty() || !capture || !restore || name.find(' ') != std::string::npos)
        return false;
    return SnapshotParticipantRegistry().emplace(
        std::move(name), SnapshotParticipant{ std::move(capture), std::move(restore) }).second;
}


class SelfTestSuite final : public Suite
{
public:
    void Start(Context& context) override
    {
        Player* actor = context.GetActor();
        context.Expect(actor != nullptr, "headless actor available");
        if (!actor)
        {
            context.Finish();
            return;
        }

        _first = context.SpawnDummy(3.0f, -0.35f);
        _second = context.SpawnDummy(3.0f, 0.35f);
        context.Expect(_first != nullptr && _second != nullptr, "two deterministic dummies spawned");
        if (!_first || !_second)
        {
            context.Finish();
            return;
        }

        _firstGuid = _first->GetGUID();
        _secondGuid = _second->GetGUID();
        context.Expect(context.GetCreature(_firstGuid) == _first && context.GetUnit(_secondGuid) == _second,
            "GUID lookup resolves live objects");
        context.Expect(context.Engage(_firstGuid) && context.Engage(_secondGuid),
            "dummies engaged without autonomous AI");
        context.Expect(context.Damage(_firstGuid, 111) && context.Damage(_secondGuid, 222),
            "controlled damage applied");

        actor->SetHealth(std::max<uint32>(1, actor->GetMaxHealth() / 2));
        actor->ModifyMoney(12345);
        actor->_AddSpellCooldown(133, 0, 0, 60000);
        if (!actor->HasSpell(1752))
            actor->learnSpell(1752, false);
        bool stored = actor->StoreNewItemInBestSlots(6948, 1);
        context.Expect(stored, "inventory mutation created");
        _waited = 0;
    }

    void Update(Context& context, uint32 diff) override
    {
        _waited += diff;
        if (_waited < 250)
            return;

        uint32 damageEvents = 0;
        for (Event const& event : context.GetEvents())
            if (event.type == EventType::Damage && (event.target == _firstGuid || event.target == _secondGuid))
                ++damageEvents;
        context.Expect(damageEvents >= 2, "damage events observed", "count=" + std::to_string(damageEvents));
        context.Expect(context.GetCreature(_firstGuid) != nullptr && context.GetCreature(_secondGuid) != nullptr,
            "dummy handles reacquired after updates");
        context.Finish();
    }

private:
    Creature* _first = nullptr;
    Creature* _second = nullptr;
    ObjectGuid _firstGuid;
    ObjectGuid _secondGuid;
    uint32 _waited = 0;
};

class HarnessWorldScript final : public WorldScript
{
public:
    HarnessWorldScript() : WorldScript("HarnessWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE,
        WORLDHOOK_ON_SHUTDOWN
    }) { }

    void OnAfterConfigLoad(bool reload) override { Manager::Instance().LoadConfig(reload); }
    void OnStartup() override { Manager::Instance().Startup(); }
    void OnUpdate(uint32 diff) override { Manager::Instance().Update(diff); }
    void OnShutdown() override { Manager::Instance().Shutdown(); }
};

class HarnessSpellScript final : public AllSpellScript
{
public:
    HarnessSpellScript() : AllSpellScript("HarnessSpellScript", {
        ALLSPELLHOOK_ON_SPELL_CHECK_CAST,
        ALLSPELLHOOK_ON_CAST,
        ALLSPELLHOOK_ON_CAST_CANCEL
    }) { }

    void OnSpellCheckCast(Spell* spell, bool strict, SpellCastResult& result) override
    {
        if (!spell || !spell->GetCaster())
            return;
        Manager::Instance().Observe({ 0, 0, EventType::CastCheck, spell->GetCaster()->GetGUID(),
            spell->m_targets.GetUnitTargetGUID(), spell->GetSpellInfo()->Id, strict ? 1 : 0, uint32(result) });
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool skipCheck) override
    {
        if (!spell || !caster || !spellInfo)
            return;
        Manager::Instance().Observe({ 0, 0, EventType::Cast, caster->GetGUID(),
            spell->m_targets.GetUnitTargetGUID(), spellInfo->Id, skipCheck ? 1 : 0, SPELL_CAST_OK });
    }

    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool bySelf) override
    {
        if (!spell || !caster || !spellInfo)
            return;
        Manager::Instance().Observe({ 0, 0, EventType::CastCancel, caster->GetGUID(),
            spell->m_targets.GetUnitTargetGUID(), spellInfo->Id, bySelf ? 1 : 0, 0 });
    }
};

class HarnessUnitScript final : public UnitScript
{
public:
    HarnessUnitScript() : UnitScript("HarnessUnitScript", true, {
        UNITHOOK_ON_HEAL_FINAL,
        UNITHOOK_ON_DAMAGE,
        UNITHOOK_ON_DAMAGE_FINAL,
        UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
        UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
        UNITHOOK_MODIFY_HEAL_RECEIVED
    }) { }

    void OnHealFinal(HealInfo const& healInfo) override
    {
        Unit* healer = healInfo.GetHealer();
        Unit* receiver = healInfo.GetTarget();
        if (healer && receiver)
            Manager::Instance().Observe({ 0, 0, EventType::Heal, healer->GetGUID(),
                receiver->GetGUID(), healInfo.GetSpellInfo() ? healInfo.GetSpellInfo()->Id : 0,
                healInfo.GetEffectiveHeal(), 0 });
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (attacker && victim)
            Manager::Instance().Observe({ 0, 0, EventType::Damage, attacker->GetGUID(), victim->GetGUID(), 0, damage, 0 });
    }
    void OnDamageFinal(Unit* attacker, Unit* victim, uint32 damage, DamageEffectType damageType,
        SpellInfo const* spellInfo, Spell const* /*damageSpell*/) override
    {
        if (attacker && victim)
            Manager::Instance().Observe({ 0, 0, EventType::DamageFinal, attacker->GetGUID(), victim->GetGUID(),
                spellInfo ? spellInfo->Id : 0, damage, uint32(damageType) });
    }


    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (attacker && target)
            Manager::Instance().Observe({ 0, 0, EventType::PeriodicDamage, attacker->GetGUID(), target->GetGUID(),
                spellInfo ? spellInfo->Id : 0, damage, 0 });
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (attacker && target)
            Manager::Instance().Observe({ 0, 0, EventType::SpellDamage, attacker->GetGUID(), target->GetGUID(),
                spellInfo ? spellInfo->Id : 0, damage, 0 });
    }

    void ModifyHealReceived(Unit* target, Unit* healer, uint32& heal, SpellInfo const* spellInfo) override
    {
        if (healer && target)
            Manager::Instance().Observe({ 0, 0, EventType::SpellHeal, healer->GetGUID(), target->GetGUID(),
                spellInfo ? spellInfo->Id : 0, heal, 0 });
    }
};

using namespace Acore::ChatCommands;

class HarnessCommandScript final : public CommandScript
{
public:
    HarnessCommandScript() : CommandScript("HarnessCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable harnessCommands = {
            { "status", HandleStatus, SEC_ADMINISTRATOR, Console::Yes },
            { "actor", HandleActor, SEC_ADMINISTRATOR, Console::Yes },
            { "begin", HandleBegin, SEC_ADMINISTRATOR, Console::Yes },
            { "rollback", HandleRollback, SEC_ADMINISTRATOR, Console::Yes },
            { "commit", HandleCommit, SEC_ADMINISTRATOR, Console::Yes },
            { "reset", HandleReset, SEC_ADMINISTRATOR, Console::Yes },
            { "dummy", HandleDummy, SEC_ADMINISTRATOR, Console::Yes },
            { "events", HandleEvents, SEC_ADMINISTRATOR, Console::Yes },
            { "run", HandleRun, SEC_ADMINISTRATOR, Console::Yes },
            { "suites", HandleSuites, SEC_ADMINISTRATOR, Console::Yes }
        };
        static ChatCommandTable root = {
            { "testharness", harnessCommands }
        };
        return root;
    }

    static bool HandleStatus(ChatHandler* handler, char const*)
    {
        handler->SendSysMessage("[TestHarness] " + Manager::Instance().Status());
        return true;
    }

    static bool HandleActor(ChatHandler* handler, char const* args)
    {
        std::string action = args ? args : "";
        std::string message;
        bool result = false;
        if (action == "start") result = Manager::Instance().StartActor(message);
        else if (action == "stop") result = Manager::Instance().StopActor(message);
        else
        {
            handler->SendSysMessage("Usage: testharness actor <start|stop>");
            return false;
        }
        handler->SendSysMessage(std::string("[TestHarness] ") + (result ? "OK " : "FAIL ") + message);
        return result;
    }

    static bool HandleBegin(ChatHandler* handler, char const* args)
    {
        std::string message;
        std::string reason = args && *args ? args : "manual";
        bool result = Manager::Instance().BeginTransaction(reason, message);
        handler->SendSysMessage(std::string("[TestHarness] ") + (result ? "OK " : "FAIL ") + message);
        return result;
    }

    static bool HandleRollback(ChatHandler* handler, char const*)
    {
        bool result = Manager::Instance().RollbackTransaction(true);
        handler->SendSysMessage(std::string("[TestHarness] ") + (result ? "OK rollback queued" : "FAIL rollback failed"));
        return result;
    }

    static bool HandleCommit(ChatHandler* handler, char const*)
    {
        std::string message;
        bool result = Manager::Instance().CommitTransaction(message);
        handler->SendSysMessage(std::string("[TestHarness] ") + (result ? "OK " : "FAIL ") + message);
        return result;
    }

    static bool HandleReset(ChatHandler* handler, char const*)
    {
        std::string message;
        bool result = Manager::Instance().ResetActor(message);
        handler->SendSysMessage(std::string("[TestHarness] ") + (result ? "OK " : "FAIL ") + message);
        return result;
    }

    static bool HandleDummy(ChatHandler* handler, char const* args)
    {
        std::istringstream input(args ? args : "");
        std::string action;
        input >> action;
        if (action == "spawn")
        {
            uint32 count = 1;
            float distance = 3.0f;
            uint32 entry = 0;
            input >> count >> distance >> entry;
            count = std::clamp<uint32>(count, 1, 32);
            uint32 spawned = 0;
            for (uint32 index = 0; index < count; ++index)
            {
                float angle = count == 1 ? 0.0f : -0.7f + 1.4f * float(index) / float(count - 1);
                if (Creature* dummy = Manager::Instance().SpawnDummy(distance, angle, entry))
                {
                    ++spawned;
                    handler->SendSysMessage("[TestHarnessDummy] guid=" + GuidText(dummy->GetGUID())
                        + " entry=" + std::to_string(dummy->GetEntry()));
                }
            }
            return spawned == count;
        }
        if (action == "clear")
        {
            Manager::Instance().DespawnAllDummies();
            handler->SendSysMessage("[TestHarness] OK dummies cleared");
            return true;
        }

        handler->SendSysMessage("Usage: testharness dummy <spawn [count] [distance] [entry]|clear>");
        return false;
    }

    static bool HandleEvents(ChatHandler* handler, char const* args)
    {
        uint64 since = 0;
        if (args && *args)
            if (Optional<uint64> parsed = Acore::StringTo<uint64>(args))
                since = *parsed;
        Manager::Instance().PrintEvents(handler, since);
        return true;
    }

    static bool HandleRun(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->SendSysMessage("Usage: testharness run <suite>");
            return false;
        }
        std::string message;
        bool result = Manager::Instance().StartRun(args, message);
        handler->SendSysMessage(std::string("[TestHarness] ") + (result ? "OK " : "FAIL ") + message);
        return result;
    }

    static bool HandleSuites(ChatHandler* handler, char const*)
    {
        for (std::string const& suite : RegisteredSuites())
            handler->SendSysMessage("[TestHarnessSuite] " + suite);
        for (std::string const& group : RegisteredSuiteGroups())
            handler->SendSysMessage("[TestHarnessSuiteGroup] " + group);
        return true;
    }
};
}

void AddSC_test_harness()
{
    TestHarness::RegisterSuite("selftest", [] { return std::make_unique<TestHarness::SelfTestSuite>(); });
    new TestHarness::HarnessWorldScript();
    new TestHarness::HarnessSpellScript();
    new TestHarness::HarnessUnitScript();
    new TestHarness::HarnessCommandScript();
}
