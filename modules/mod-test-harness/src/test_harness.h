/*
 * Reusable, headless integration-test harness for AzerothCore modules.
 */
#ifndef MOD_TEST_HARNESS_H
#define MOD_TEST_HARNESS_H

#include "DatabaseEnvFwd.h"
#include "ObjectGuid.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class Creature;
class Player;
class Unit;

namespace TestHarness
{
enum class EventType : uint8
{
    CastCheck,
    Cast,
    CastCancel,
    Damage,
    DamageFinal,
    Heal,
    SpellDamage,
    PeriodicDamage,
    SpellHeal
};

struct Event
{
    uint64 sequence = 0;
    uint32 timestampMs = 0;
    EventType type = EventType::Damage;
    ObjectGuid source;
    ObjectGuid target;
    uint32 spellId = 0;
    int64 amount = 0;
    uint32 result = 0;
};

struct Assertion
{
    bool passed = false;
    std::string name;
    std::string detail;
};

class Context
{
public:
    Player* GetActor() const;
    Unit* GetUnit(ObjectGuid guid) const;
    Creature* GetCreature(ObjectGuid guid) const;

    Creature* SpawnDummy(float distance = 3.0f, float angleOffset = 0.0f, uint32 entry = 0);
    bool DespawnDummy(ObjectGuid guid);
    void DespawnAllDummies();
    bool Engage(ObjectGuid guid, float threat = 1.0f);
    bool Damage(ObjectGuid guid, uint32 amount);

    std::vector<Event> const& GetEvents() const;
    Event const* FindEvent(EventType type, ObjectGuid source = ObjectGuid::Empty,
        ObjectGuid target = ObjectGuid::Empty, uint32 spellId = 0) const;
    std::size_t CountEvents(EventType type, ObjectGuid source = ObjectGuid::Empty,
        ObjectGuid target = ObjectGuid::Empty, uint32 spellId = 0) const;
    void ClearEvents();

    void Expect(bool condition, std::string name, std::string detail = {});
    void Finish();
    void Fail(std::string name, std::string detail = {});
    bool IsFinished() const;
    uint32 ElapsedMs() const;

private:
    friend class Manager;
    Context();

    std::vector<Assertion> _assertions;
    bool _finished = false;
    uint64 _startedAtMs = 0;
};

class Suite
{
public:
    virtual ~Suite() = default;
    virtual void Start(Context& context) = 0;
    virtual void Update(Context& context, uint32 diff) = 0;
    virtual bool UsesTransaction() const { return true; }
    virtual void Cancel(Context& /*context*/) { }
};

using SuiteFactory = std::function<std::unique_ptr<Suite>()>;

using SnapshotCapture = std::function<bool(ObjectGuid::LowType guid, std::string& payload)>;
struct SnapshotRestorePlan
{
    std::function<bool(CharacterDatabaseTransaction const& transaction)> append;
    std::function<void()> commit;
};
using SnapshotRestore =
    std::function<std::optional<SnapshotRestorePlan>(ObjectGuid::LowType guid, std::string_view payload)>;
// Registration is intended for module static initialization or AddSC_* loaders.
bool RegisterSuite(std::string name, SuiteFactory factory);
bool UnregisterSuite(std::string_view name);
std::vector<std::string> RegisteredSuites();
bool RegisterSuiteGroup(std::string name, std::vector<std::string> suites);
std::vector<std::string> RegisteredSuiteGroups();
bool RegisterSnapshotParticipant(std::string name, SnapshotCapture capture, SnapshotRestore restore);
}

#endif
