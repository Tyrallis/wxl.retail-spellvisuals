// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// wxl.retail-spellvisuals: retail spell visuals, played as retail plays them.
//
// The Retroport Workbench writes DBFilesClient\WXLRetailSpellVisuals.txt into the client's
// patch-S.MPQ: for each new spell, retail's whole visual -- its events (a moment of the cast to
// start at and one to end at, on the caster, the target or the ground, with their delays) and the
// models each event shows (at an attachment point, with an offset, a turn, a scale, a delay and
// animations), and its missiles. The spell's own 3.3.5 SpellVisual keeps only the caster's
// animations. This mod listens to every cast in view and plays the table's events:
//
//   retail moment                      3.3.5 event that starts it
//   1  precast start                   SpellCastStart (cast time > 0)
//   2  precast end                     SpellCastGo, or the cast failing
//   3  cast, 4 travel start            SpellCastGo
//   5  travel end, 6 impact            SpellCastGo + the missile's flight (distance / speed)
//   7  aura start, 8 aura end          UnitAuraChanged; a ground aura: its dynamic object
//   9  area start, 10 area end         a dynamic object of the spell appearing / leaving
//   11 channel start, 12 channel end   SpellChannelStart / SpellChannelStop
//   13 (as an end) played once         the model's animation has played once
//
// Models hang on the unit's model at retail's attachment point (a point 3.3.5's body lacks falls
// back to the chest, then the body's origin) or stand in the world at the ground point, turned
// with the caster. Missiles fly from the caster's point to the target's, at retail's speed.
//
// Body glows (Glow.cpp): an event whose kit colours the unit itself (EdgeGlowEffect, ShadowyEffect,
// GradientEffect, OutlineEffect -- Avenging Wrath's gold, Combustion's fire, Bestial Wrath's red) has
// `glow` lines; the unit glows from the event's start moment to its end, fading in and out.
//
// Not carried yet: sounds, scripted model paths (positioners), camera shakes.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wxl/objects.hpp"
#include "wxl/sdk.hpp"
#include "wxl/services/m2.h"
#include "wxl/services/storage.hpp"

#include "Glow.hpp"

namespace
{
    constexpr const char* kTag       = "retail-spellvisuals";
    constexpr const char* kTablePath = "DBFilesClient\\WXLRetailSpellVisuals.txt";

    wxl::Service<wxl_m2_api>  g_m2(WXL_M2_SERVICE, WXL_M2_SERVICE_VERSION);
    const wxl::StorageService g_storage;

    bool g_verbose = false;

    void Logf(wxl_log_level level, const char* format, ...)
    {
        char    line[512];
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof line, format, args);
        va_end(args);
        wxl::Log(level, kTag, line);
    }

#define VLOG(...)                                   \
    do                                              \
    {                                               \
        if (g_verbose) Logf(WXL_LOG_INFO, __VA_ARGS__); \
    } while (0)

    /// Whether the runtime's wxl.m2 table carries `member`: the world calls are newer than it.
    template <class T>
    bool Carries(T wxl_m2_api::* member)
    {
        const wxl_m2_api* api = g_m2.Get();
        if (!api) return false;
        const size_t at = reinterpret_cast<size_t>(&(static_cast<const wxl_m2_api*>(nullptr)->*member));
        return api->struct_size >= at + sizeof(T) && api->*member != nullptr;
    }

    uint64_t NowMs()
    {
        using namespace std::chrono;
        return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    std::mt19937& Rng()
    {
        static std::mt19937 rng{std::random_device{}()};
        return rng;
    }

    uint32_t Between(uint32_t lo, uint32_t hi)
    {
        if (hi <= lo) return lo;
        return std::uniform_int_distribution<uint32_t>(lo, hi)(Rng());
    }

    std::string ModDirectory()
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&ModDirectory), &self))
            return {};
        char path[MAX_PATH];
        const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
        std::string dir(path, n);
        // <mod>\bin\x64\wxl-retail-spellvisuals.dll -> <mod>
        for (int up = 0; up < 3; ++up)
        {
            const size_t slash = dir.find_last_of("\\/");
            if (slash == std::string::npos) return {};
            dir.resize(slash);
        }
        return dir;
    }

    void LoadSettings()
    {
        const std::string ini = ModDirectory() + "\\RetailSpellVisuals.ini";
        g_verbose = GetPrivateProfileIntA("Log", "Verbose", 0, ini.c_str()) != 0;
    }

    // --- The table ----------------------------------------------------------------------------

    enum Moment : uint32_t
    {
        PrecastStart = 1,
        PrecastEnd   = 2,
        CastGo       = 3,
        TravelStart  = 4,
        TravelEnd    = 5,
        Impact       = 6,
        AuraStart    = 7,
        AuraEnd      = 8,
        AreaStart    = 9,
        AreaEnd      = 10,
        ChannelStart = 11,
        ChannelEnd   = 12,
        OneShot      = 13,
    };

    enum Target : uint32_t
    {
        OnCaster = 1,
        OnTarget = 2,
        OnGround = 3,
    };

    /// What a retail effect model plays when its kit names no animation: Stand once as its
    /// intro, Hold as long as its event lasts, Decay as it goes (AnimationData 0, 158, 159).
    constexpr uint32_t kStand = 0;
    constexpr uint32_t kHold  = 158;
    constexpr uint32_t kDecay = 159;

    /// SpellVisualKitModelAttach flag: the model is sized to its area trigger's radius.
    constexpr uint32_t kScalesWithArea = 0x8;

    struct ModelDef
    {
        std::string path;
        int32_t     attachment = -1;
        float       offset[3]{};
        float       turn[3]{}; // yaw, pitch, roll
        float       scale = 1.0f;
        uint32_t    delay = 0;
        int32_t     startAnim = -1, anim = -1, endAnim = -1;
        float       alpha = 1.0f;
        uint32_t    flags = 0;
        bool        upright = false; ///< at its point but turned as the unit faces (retail's positioner), not with the bone
        // an area model (SpellVisualKitAreaModel): copies scattered over the area while it lasts
        float       rate = 0.0f;     ///< copies per second
        uint32_t    lifetime = 0;    ///< ms each lasts; 0: its own animation, once
    };

    struct EventDef
    {
        uint32_t              start = 0, end = 0;
        uint32_t              startMin = 0, startMax = 0, endMin = 0, endMax = 0;
        uint32_t              target = OnCaster;
        uint32_t              kitMin = 0, kitMax = 0;
        std::vector<ModelDef> models;
        std::vector<ModelDef> areas;
        std::vector<glow::Params> glows; ///< what the kit does to the unit's own body
    };

    struct MissileDef
    {
        std::string path;
        int32_t     attachment = -1, destination = -1;
        float       castOffset[3]{}, impactOffset[3]{};
        float       scale = 1.0f;
        float       speed = 0.0f;
    };

    struct SpellDef
    {
        uint32_t                id = 0, retail = 0, cast = 0, duration = 0, launch = 0;
        float                   speed = 0.0f;
        std::vector<MissileDef> missiles;
        std::vector<EventDef>   events;

        bool Has(uint32_t moment) const
        {
            for (const EventDef& e : events)
                if (e.start == moment) return true;
            return false;
        }
    };

    std::unordered_map<uint32_t, SpellDef> g_spells;
    bool                                   g_tableRead = false;

    std::vector<std::string_view> Split(std::string_view line)
    {
        std::vector<std::string_view> out;
        size_t at = 0;
        while (at <= line.size())
        {
            size_t tab = line.find('\t', at);
            if (tab == std::string_view::npos) tab = line.size();
            out.push_back(line.substr(at, tab - at));
            at = tab + 1;
        }
        return out;
    }

    float F(std::string_view s) { return std::strtof(std::string(s).c_str(), nullptr); }
    int32_t I(std::string_view s) { return static_cast<int32_t>(std::strtol(std::string(s).c_str(), nullptr, 10)); }
    uint32_t U(std::string_view s) { const int32_t v = I(s); return v < 0 ? 0u : static_cast<uint32_t>(v); }

    void ReadTable()
    {
        g_tableRead = true;
        g_spells.clear();
        if (!g_storage.Exists(kTablePath))
        {
            Logf(WXL_LOG_INFO, "%s is not in the client: no retail spell visuals to play", kTablePath);
            return;
        }
        const wxl::Expected<std::string> text = g_storage.ReadText(kTablePath);
        if (!text)
        {
            Logf(WXL_LOG_WARN, "could not read %s", kTablePath);
            return;
        }
        SpellDef* spell = nullptr;
        EventDef* event = nullptr;
        size_t    models = 0;
        std::string_view all = *text;
        while (!all.empty())
        {
            size_t nl = all.find('\n');
            std::string_view line = all.substr(0, nl);
            all = nl == std::string_view::npos ? std::string_view{} : all.substr(nl + 1);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || line[0] == '#') continue;
            const std::vector<std::string_view> f = Split(line);
            if (f[0] == "spell" && f.size() >= 7)
            {
                SpellDef def;
                def.id       = U(f[1]);
                def.retail   = U(f[2]);
                def.cast     = U(f[3]);
                def.duration = U(f[4]);
                def.speed    = F(f[5]);
                def.launch   = U(f[6]);
                spell = &(g_spells[def.id] = def);
                event = nullptr;
            }
            else if (f[0] == "missile" && f.size() >= 13 && spell)
            {
                MissileDef m;
                m.path        = std::string(f[1]);
                m.attachment  = I(f[2]);
                m.destination = I(f[3]);
                for (int k = 0; k < 3; ++k)
                {
                    m.castOffset[k]   = F(f[4 + k]);
                    m.impactOffset[k] = F(f[7 + k]);
                }
                m.scale = F(f[10]);
                m.speed = f.size() > 12 ? F(f[12]) : 0.0f;
                spell->missiles.push_back(std::move(m));
            }
            else if (f[0] == "event" && f.size() >= 10 && spell)
            {
                EventDef e;
                e.start    = U(f[1]);
                e.end      = U(f[2]);
                e.startMin = U(f[3]);
                e.startMax = U(f[4]);
                e.endMin   = U(f[5]);
                e.endMax   = U(f[6]);
                e.target   = U(f[7]);
                e.kitMin   = U(f[8]);
                e.kitMax   = U(f[9]);
                spell->events.push_back(std::move(e));
                event = &spell->events.back();
            }
            else if (f[0] == "model" && f.size() >= 16 && event)
            {
                ModelDef m;
                m.path       = std::string(f[1]);
                m.attachment = I(f[2]);
                for (int k = 0; k < 3; ++k)
                {
                    m.offset[k] = F(f[3 + k]);
                    m.turn[k]   = F(f[6 + k]);
                }
                m.scale     = F(f[9]);
                m.delay     = U(f[10]);
                m.startAnim = I(f[11]);
                m.anim      = I(f[12]);
                m.endAnim   = I(f[13]);
                m.alpha     = F(f[14]);
                m.flags     = f.size() > 15 ? U(f[15]) : 0;
                m.upright   = f.size() > 16 && U(f[16]) != 0;
                if (m.scale <= 0.0f) m.scale = 1.0f;
                if (m.alpha <= 0.0f) m.alpha = 1.0f;
                event->models.push_back(std::move(m));
                ++models;
            }
            else if (f[0] == "glow" && f.size() >= 20 && event)
            {
                // glow  fade_in  fade_out  c0(3) c1(3) c2(3)  alpha_middle  alpha_edge  power  halo(3)  halo_px  halo_alpha
                glow::Params g;
                g.fadeIn  = U(f[1]);
                g.fadeOut = U(f[2]);
                for (int c = 0; c < 3; ++c)
                    for (int k = 0; k < 3; ++k) g.colors[c][k] = F(f[3 + c * 3 + k]);
                g.alphaMiddle = F(f[12]);
                g.alphaEdge   = F(f[13]);
                g.power       = F(f[14]);
                for (int k = 0; k < 3; ++k) g.halo[k] = F(f[15 + k]);
                g.haloPx    = F(f[18]);
                g.haloAlpha = F(f[19]);
                event->glows.push_back(g);
            }
            else if (f[0] == "area" && f.size() >= 6 && event)
            {
                // area  path  rate  lifetime  scale  flags
                ModelDef m;
                m.path     = std::string(f[1]);
                m.rate     = F(f[2]);
                m.lifetime = U(f[3]);
                m.scale    = F(f[4]);
                m.flags    = U(f[5]);
                if (m.scale <= 0.0f) m.scale = 1.0f;
                if (m.rate > 0.0f) event->areas.push_back(std::move(m));
            }
        }
        Logf(WXL_LOG_INFO, "%zu spell(s) with retail visuals, %zu model(s)", g_spells.size(), models);
    }

    const SpellDef* Def(uint32_t spell)
    {
        if (!g_tableRead) ReadTable();
        auto it = g_spells.find(spell);
        return it == g_spells.end() ? nullptr : &it->second;
    }

    // --- Where things are -----------------------------------------------------------------------

    struct Vec
    {
        float x = 0, y = 0, z = 0;
    };

    Vec ToVec(const wxl_vec3& v) { return {v.x, v.y, v.z}; }

    bool PositionOf(uint64_t guid, Vec& out, float* facing = nullptr)
    {
        const wxl::Object object = wxl::Object::Resolve(guid);
        if (!object.HasHandle()) return false;
        const wxl::Expected<wxl_vec3> at = object.Position();
        if (!at) return false;
        out = ToVec(*at);
        if (facing)
        {
            const wxl::Expected<float> f = object.Facing();
            *facing = f ? *f : 0.0f;
        }
        return true;
    }

    void* ModelOf(uint64_t guid)
    {
        const wxl::Object object = wxl::Object::Resolve(guid);
        return object.HasHandle() ? g_m2->object_model(object.Handle()) : nullptr;
    }

    /// The world position of a unit's attachment point, else its position raised to about the chest.
    bool PointOf(uint64_t guid, int32_t attachment, Vec& out)
    {
        void* body = ModelOf(guid);
        wxl_vec3 at{};
        if (body && attachment >= 0 && g_m2->attachment_world(body, static_cast<uint32_t>(attachment), &at) == WXL_OK)
        {
            out = ToVec(at);
            return true;
        }
        if (body && g_m2->attachment_world(body, 15, &at) == WXL_OK) // chest
        {
            out = ToVec(at);
            return true;
        }
        if (!PositionOf(guid, out)) return false;
        out.z += 1.5f;
        return true;
    }

    /// Retail's offset, in the frame of something facing `facing`: 3.3.5's spell effects read the
    /// side axis mirrored (CEffect::ApplyModelAttachRecToMatrix), and retail's tables keep that.
    Vec Turned(const float offset[3], float facing)
    {
        const float c = std::cos(facing), s = std::sin(facing);
        const float x = offset[0], y = -offset[1];
        return {x * c - y * s, x * s + y * c, offset[2]};
    }

    // --- What is shown --------------------------------------------------------------------------

    /// One cast in progress or finished, or one aura or area effect: what its events hang off.
    struct Cast
    {
        uint32_t        serial = 0;
        const SpellDef* def    = nullptr;
        uint64_t        caster = 0;
        uint64_t        target = 0;
        std::vector<uint64_t> hits;
        bool            hasDest = false;
        Vec             dest;
        float           facing = 0.0f; // the caster's, when the cast started
        float           radius = 0.0f; // an area effect's: its dynamic object's radius
        uint64_t        born   = 0;
        uint64_t        lastUse = 0;
    };

    enum class State
    {
        Waiting, ///< not shown yet: its start delay runs
        Shown,
        Ending,  ///< its end animation, or its particles, play out
    };

    struct Live
    {
        uint32_t        cast = 0;  ///< Cast::serial
        const EventDef* event = nullptr;
        const ModelDef* def   = nullptr;
        void*           model = nullptr;
        uint64_t        unit  = 0;      ///< hangs on this unit; 0 stands in the world
        Vec             at;             ///< in the world: where
        float           yaw = 0.0f;     ///< in the world: turned with this facing
        float           size = 1.0f;    ///< times the model's scale: an area model, the area's radius
        State           state = State::Waiting;
        uint64_t        showAt = 0;
        uint64_t        endAt  = 0;     ///< when it starts ending; 0 until known
        uint64_t        goneAt = 0;     ///< when it is released, once ending
        bool            oneShot = false;
        bool            placed  = false; ///< hung on its unit, or stood in the world
        bool            looping = false; ///< past its intro, in its loop
        uint64_t        loopAt  = 0;     ///< when its intro ends; 0 until the model has loaded
        int32_t         intro   = -1;    ///< the animation it started with, -1 for none (straight to its loop)
    };

    struct Missile
    {
        uint32_t          cast = 0;
        const MissileDef* def  = nullptr;
        void*             model = nullptr;
        Vec               from, to;
        uint64_t          target = 0; ///< flies to this unit, followed; 0 flies to `to`
        uint64_t          launched = 0;
        uint64_t          arrives  = 0;
    };

    std::unordered_map<uint32_t, Cast> g_casts;
    std::unordered_map<uint64_t, uint32_t> g_castOf; ///< the caster's cast in progress
    std::vector<Live>                  g_lives;
    std::vector<Missile>               g_missiles;

    /// An area model being scattered: a copy every 1/rate s at a random point of the area.
    struct Emitter
    {
        uint32_t        cast  = 0;
        const EventDef* event = nullptr;
        const ModelDef* def   = nullptr;
        Vec             center;
        float           radius = 4.0f;
        uint64_t        next   = 0;
        uint64_t        endAt  = 0; ///< 0 until its event ends
    };
    std::vector<Emitter> g_emitters;

    /// A unit glowing for an event: from its start (fading in) until the event ends (fading out).
    struct LiveGlow
    {
        uint32_t            cast  = 0;
        const EventDef*     event = nullptr;
        const glow::Params* def   = nullptr;
        uint64_t            unit  = 0;
        uint64_t            startAt = 0;
        uint64_t            endAt   = 0; ///< 0 until its event ends
    };
    std::vector<LiveGlow> g_glows;
    uint32_t                           g_nextSerial = 1;

    struct Timed
    {
        uint64_t at;
        uint32_t cast;
        uint32_t moment;
    };
    std::vector<Timed> g_timed; ///< moments still to come: impacts after a flight

    /// Auras and area effects: (unit or dynamic object guid, spell) -> the cast they hang off.
    struct AuraKey
    {
        uint64_t guid;
        uint32_t spell;
        bool operator==(const AuraKey& o) const { return guid == o.guid && spell == o.spell; }
    };
    struct AuraHash
    {
        size_t operator()(const AuraKey& k) const { return std::hash<uint64_t>{}(k.guid * 31 + k.spell); }
    };
    std::unordered_map<AuraKey, uint32_t, AuraHash> g_auraOf;


    Cast& NewCast(const SpellDef* def, uint64_t caster)
    {
        const uint32_t serial = g_nextSerial++;
        Cast& c = g_casts[serial];
        c.serial = serial;
        c.def    = def;
        c.caster = caster;
        c.born = c.lastUse = NowMs();
        PositionOf(caster, c.dest, &c.facing);
        return c;
    }

    void Release(void*& model)
    {
        if (model) g_m2->release_model(model);
        model = nullptr;
    }

    /// Where an event's models go: each unit it hangs on (0 for the ground), and the ground point.
    void Places(const Cast& cast, uint32_t target, std::vector<uint64_t>& units, bool& ground)
    {
        units.clear();
        ground = false;
        switch (target)
        {
        case OnCaster: units.push_back(cast.caster); return;
        case OnGround: ground = true; return;
        default:
            if (!cast.hits.empty()) units = cast.hits;
            else if (cast.target) units.push_back(cast.target);
            else if (cast.hasDest) ground = true;
            else units.push_back(cast.caster);
            return;
        }
    }

    /// The point a ground event stands at: the cast's destination, else its target, else the caster.
    Vec GroundOf(const Cast& cast)
    {
        if (cast.hasDest) return cast.dest;
        Vec at;
        if (cast.target && PositionOf(cast.target, at)) return at;
        if (PositionOf(cast.caster, at)) return at;
        return cast.dest;
    }

    /// Starts every event of the cast that begins at `moment`.
    void Fire(Cast& cast, uint32_t moment)
    {
        const uint64_t now = NowMs();
        cast.lastUse = now;
        std::vector<uint64_t> units;
        for (const EventDef& e : cast.def->events)
        {
            if (e.start != moment) continue;
            bool ground = false;
            Places(cast, e.target, units, ground);
            const uint64_t start = now + Between(e.startMin, e.startMax) + Between(e.kitMin, e.kitMax);
            const bool oneShot = e.end == OneShot || e.end == 0 || e.end == e.start;
            const Vec  point   = ground ? GroundOf(cast) : Vec{};
            VLOG("spell %u: moment %u -> %zu model(s) on %s", cast.def->id, moment, e.models.size(),
                 ground ? "the ground" : e.target == OnCaster ? "the caster" : "the target");
            if (!e.areas.empty())
            {
                // retail scatters these over the area trigger; on a unit, around it
                Vec center = point;
                if (!ground && !units.empty()) PositionOf(units.front(), center);
                for (const ModelDef& a : e.areas)
                {
                    Emitter em;
                    em.cast   = cast.serial;
                    em.event  = &e;
                    em.def    = &a;
                    em.center = center;
                    em.radius = cast.radius > 0.0f ? cast.radius : 4.0f;
                    em.next   = start;
                    // a one-shot event scatters for the spell's duration, else a second
                    if (oneShot) em.endAt = start + (cast.def->duration ? cast.def->duration : 1000);
                    g_emitters.push_back(em);
                }
            }
            if (!ground)
                for (const glow::Params& g : e.glows)
                    for (uint64_t unit : units)
                    {
                        LiveGlow lg;
                        lg.cast    = cast.serial;
                        lg.event   = &e;
                        lg.def     = &g;
                        lg.unit    = unit;
                        lg.startAt = start;
                        // a one-shot event glows for a moment: its fade in, a second, its fade out
                        if (oneShot) lg.endAt = start + g.fadeIn + 1000;
                        g_glows.push_back(lg);
                    }
            for (const ModelDef& m : e.models)
            {
                auto add = [&](uint64_t unit) {
                    Live live;
                    live.cast    = cast.serial;
                    live.event   = &e;
                    live.def     = &m;
                    live.unit    = unit;
                    live.at      = point;
                    live.yaw     = cast.facing;
                    // An area's ground model (kit flag 0x8) is made one yard across its radius and
                    // stretched to the area's, as retail sizes it to the area trigger.
                    if (ground && (m.flags & kScalesWithArea) && cast.radius > 0.0f) live.size = cast.radius;
                    live.showAt  = start + m.delay;
                    live.oneShot = oneShot;
                    g_lives.push_back(live);
                };
                if (ground) add(0);
                for (uint64_t unit : units) add(unit);
            }
        }
        // The moments that end something of this cast.
        for (LiveGlow& lg : g_glows)
            if (lg.cast == cast.serial && lg.event->end == moment && !lg.endAt)
                lg.endAt = (std::max)(now + Between(lg.event->endMin, lg.event->endMax), lg.startAt);
        for (Emitter& em : g_emitters)
            if (em.cast == cast.serial && em.event->end == moment && !em.endAt)
                em.endAt = now + Between(em.event->endMin, em.event->endMax);
        for (Live& live : g_lives)
        {
            if (live.cast != cast.serial || live.oneShot || live.event->end != moment || live.endAt) continue;
            live.endAt = now + Between(live.event->endMin, live.event->endMax);
            if (live.endAt < live.showAt) live.endAt = live.showAt;
        }
    }

    /// Ends everything of a cast that has not ended yet: a cast that failed or was interrupted.
    void EndAll(uint32_t serial, bool drop_waiting)
    {
        const uint64_t now = NowMs();
        for (Live& live : g_lives)
        {
            if (live.cast != serial) continue;
            if (live.state == State::Waiting && drop_waiting) live.endAt = live.showAt = now;
            else if (!live.endAt || live.endAt > now) live.endAt = now;
        }
        for (Timed& t : g_timed)
            if (t.cast == serial) t.at = 0, t.cast = 0;
        for (Emitter& em : g_emitters)
            if (em.cast == serial && (!em.endAt || em.endAt > now)) em.endAt = now;
        for (LiveGlow& lg : g_glows)
            if (lg.cast == serial && (!lg.endAt || lg.endAt > now)) lg.endAt = (std::max)(now, lg.startAt);
    }

    /// The glows showing now, at their fades; the ones faded out are forgotten.
    void GlowsNow(std::vector<glow::Active>& out)
    {
        const uint64_t now = NowMs();
        g_glows.erase(std::remove_if(g_glows.begin(), g_glows.end(),
                                     [&](const LiveGlow& lg) { return lg.endAt && now >= lg.endAt + lg.def->fadeOut; }),
                      g_glows.end());
        for (const LiveGlow& lg : g_glows)
        {
            if (now < lg.startAt) continue;
            float k = lg.def->fadeIn ? (std::min)(1.0f, float(now - lg.startAt) / float(lg.def->fadeIn)) : 1.0f;
            if (lg.endAt && now >= lg.endAt)
                k *= lg.def->fadeOut ? (std::max)(0.0f, 1.0f - float(now - lg.endAt) / float(lg.def->fadeOut)) : 0.0f;
            if (k > 0.0f) out.push_back({lg.unit, lg.def, k});
        }
    }

    // --- Showing a model ------------------------------------------------------------------------

    bool Hang(Live& live)
    {
        void* body = ModelOf(live.unit);
        if (!body) return false;
        const ModelDef& m = *live.def;
        wxl_m2_placement local{};
        local.struct_size = sizeof local;
        const Vec off     = Turned(m.offset, 0.0f);
        local.position    = {off.x, off.y, off.z};
        local.yaw         = -m.turn[0];
        local.pitch       = m.turn[1];
        local.roll        = m.turn[2];
        local.scale       = m.scale;
        g_m2->set_model_transform(live.model, &local);
        if (m.attachment >= 0 &&
            g_m2->attach_model(live.model, body, static_cast<uint32_t>(m.attachment), 0) == WXL_OK)
            return true;
        if (m.attachment >= 0 && g_m2->attach_model(live.model, body, 15, 0) == WXL_OK) return true;
        return g_m2->attach_model(live.model, body, 0, WXL_M2_ATTACH_ROOT) == WXL_OK;
    }

    /// An upright model on a unit: stood in the world at the unit's attachment point, turned as the
    /// unit faces (retail's positioner takes its rotation from the base, not from the point's bone:
    /// a book held in the hand stays level). Placed again every frame.
    bool Follow(Live& live)
    {
        const ModelDef& m = *live.def;
        Vec at;
        float facing = 0.0f;
        Vec root;
        if (!PositionOf(live.unit, root, &facing) || !PointOf(live.unit, m.attachment, at)) return false;
        const Vec off = Turned(m.offset, facing);
        wxl_m2_placement p{};
        p.struct_size = sizeof p;
        p.position    = {at.x + off.x, at.y + off.y, at.z + off.z};
        p.yaw         = facing - m.turn[0];
        p.pitch       = m.turn[1];
        p.roll        = m.turn[2];
        p.scale       = m.scale;
        return g_m2->place_model(live.model, &p) == WXL_OK;
    }

    /// The ground's height at (x, y) near height z; false when the ray finds none close by.
    bool GroundAt(float x, float y, float z, float& out)
    {
        if (!Carries(&wxl_m2_api::ground_height)) return false;
        const wxl_vec3 at{x, y, z};
        return g_m2->ground_height(&at, 4.0f, 8.0f, &out) == WXL_OK;
    }

    /// How far a ground model lies above the ground: enough that it never flickers into it.
    constexpr float kGroundLift = 0.06f;

    bool Stand(Live& live)
    {
        const ModelDef& m = *live.def;
        const Vec off = Turned(m.offset, live.yaw);
        wxl_m2_placement at{};
        at.struct_size = sizeof at;
        at.position    = {live.at.x + off.x, live.at.y + off.y, live.at.z + off.z};
        at.yaw         = live.yaw - m.turn[0];
        at.pitch       = m.turn[1];
        at.roll        = m.turn[2];
        at.scale       = m.scale * live.size;

        // A model retail lays flat on the ground follows the ground: sat on it at its centre and
        // tilted to the slope under it (four samples across its width), lifted a hair so the
        // terrain does not cut through it. One retail turns on purpose keeps its turn.
        float h0 = 0.0f;
        if (m.turn[1] == 0.0f && m.turn[2] == 0.0f && GroundAt(at.position.x, at.position.y, live.at.z, h0))
        {
            const float r = std::fmax(0.5f, std::fmin(12.0f, 0.75f * at.scale));
            const float cx = at.position.x, cy = at.position.y;
            float e = h0, w = h0, n = h0, so = h0;
            GroundAt(cx + r, cy, h0, e);
            GroundAt(cx - r, cy, h0, w);
            GroundAt(cx, cy + r, h0, n);
            GroundAt(cx, cy - r, h0, so);
            const float gx = (e - w) / (2.0f * r), gy = (n - so) / (2.0f * r);
            // the slope along the model's own axes, once turned
            const float c = std::cos(at.yaw), s = std::sin(at.yaw);
            const float along = gx * c + gy * s, across = -gx * s + gy * c;
            at.pitch = -std::atan(along);
            at.roll  = std::atan(across);
            at.position.z = h0 + kGroundLift + m.offset[2];
        }
        return g_m2->place_model(live.model, &at) == WXL_OK;
    }

    void Show(Live& live, uint64_t now)
    {
        const ModelDef& m = *live.def;
        live.model = g_m2->create_world_model(wxl_str{m.path.data(), static_cast<uint32_t>(m.path.size())});
        if (!live.model)
        {
            live.state  = State::Ending;
            live.goneAt = now;
            return;
        }
        // A model in the world stands there once its file has loaded, as the client's own spell
        // effects do (CEffect::PostInitialize): its bounds place it on the map.
        const bool follows = live.unit && m.upright;
        if ((live.unit && !follows) || g_m2->model_loaded(live.model))
        {
            if (!(follows ? Follow(live) : live.unit ? Hang(live) : Stand(live)))
            {
                VLOG("%s: nowhere to show it", m.path.c_str());
                Release(live.model);
                live.state  = State::Ending;
                live.goneAt = now;
                return;
            }
            live.placed = true;
        }
        if (m.alpha < 1.0f) g_m2->set_alpha(live.model, m.alpha);
        // The intro: the kit's start animation, else Stand -- unless the kit names a loop and no
        // start, which then plays from the first frame. A one-shot model plays its intro and goes.
        if (m.startAnim >= 0) live.intro = m.startAnim;
        else if (m.anim >= 0) live.intro = live.oneShot ? m.anim : -1;
        else live.intro = static_cast<int32_t>(kStand);
        const int32_t first = live.intro >= 0 ? live.intro : m.anim;
        g_m2->play_animation(live.model, static_cast<uint32_t>(first), 1.0f);
        live.looping = live.intro < 0;
        live.state   = State::Shown;
        VLOG("%s shown %s", m.path.c_str(), live.unit ? "on a unit" : "in the world");
    }

    uint32_t LengthOf(void* model, uint32_t anim)
    {
        uint32_t length = 0;
        g_m2->animation_length(model, anim, &length);
        return length;
    }

    /// Lets a shown model go: its outro (the kit's end animation, else Decay) plays once first.
    /// A model without one goes at once -- a lasting one after its particles had a moment to fade.
    void BeginEnding(Live& live, uint64_t now)
    {
        live.state = State::Ending;
        if (!live.model || !g_m2->model_loaded(live.model))
        {
            live.goneAt = now;
            return;
        }
        const uint32_t outro  = live.def->endAnim >= 0 ? static_cast<uint32_t>(live.def->endAnim) : kDecay;
        const uint32_t length = LengthOf(live.model, outro);
        if (length)
        {
            g_m2->play_animation(live.model, outro, 1.0f);
            live.goneAt = now + length;
            return;
        }
        g_m2->set_emitting(live.model, WXL_FALSE);
        live.goneAt = live.oneShot ? now : now + 500;
    }

    void Tick()
    {
        const uint64_t now = NowMs();

        // Moments that come later: impacts at the end of a flight.
        for (size_t i = 0; i < g_timed.size();)
        {
            Timed t = g_timed[i];
            if (!t.cast)
            {
                g_timed[i] = g_timed.back();
                g_timed.pop_back();
                continue;
            }
            if (t.at > now)
            {
                ++i;
                continue;
            }
            g_timed[i] = g_timed.back();
            g_timed.pop_back();
            auto it = g_casts.find(t.cast);
            if (it != g_casts.end()) Fire(it->second, t.moment);
        }

        // Area models: copies at random points of the area, each standing on the ground.
        for (Emitter& em : g_emitters)
        {
            const uint64_t step = static_cast<uint64_t>(1000.0f / std::fmax(0.1f, em.def->rate));
            for (int burst = 0; em.next <= now && (!em.endAt || em.next < em.endAt) && burst < 8; ++burst)
            {
                const float r = em.radius * std::sqrt(std::uniform_real_distribution<float>(0.0f, 1.0f)(Rng()));
                const float a = std::uniform_real_distribution<float>(0.0f, 6.2831853f)(Rng());
                Live live;
                live.cast    = em.cast;
                live.event   = em.event;
                live.def     = em.def;
                live.at      = {em.center.x + r * std::cos(a), em.center.y + r * std::sin(a), em.center.z};
                live.yaw     = std::uniform_real_distribution<float>(0.0f, 6.2831853f)(Rng());
                live.showAt  = em.next;
                live.oneShot = em.def->lifetime == 0;
                if (em.def->lifetime) live.endAt = em.next + em.def->lifetime;
                g_lives.push_back(live);
                em.next += step;
            }
            if (em.next <= now) em.next = now + step; // fell behind: no catching up
        }
        g_emitters.erase(std::remove_if(g_emitters.begin(), g_emitters.end(),
                                        [now](const Emitter& em) { return (em.endAt && em.endAt <= now) ||
                                                                          !g_casts.count(em.cast); }),
                         g_emitters.end());

        // Models.
        for (Live& live : g_lives)
        {
            if (live.state == State::Waiting)
            {
                if (live.endAt && live.endAt <= live.showAt && live.endAt <= now)
                {
                    live.state  = State::Ending;
                    live.goneAt = now;
                    continue;
                }
                if (live.showAt <= now) Show(live, now);
                continue;
            }
            if (live.state == State::Shown)
            {
                if (!live.model)
                {
                    BeginEnding(live, now);
                    continue;
                }
                // A model hanging on a unit that left goes with it.
                const bool follows = live.unit && live.def->upright;
                if (live.unit && (follows || !g_m2->model_parent(live.model)) && !ModelOf(live.unit))
                {
                    live.endAt = now;
                }
                if (follows && live.placed && !Follow(live)) live.endAt = now;
                if (!live.placed && g_m2->model_loaded(live.model))
                {
                    if (!(follows ? Follow(live) : Stand(live)))
                    {
                        live.endAt = now;
                    }
                    live.placed = true;
                }
                if (g_m2->model_loaded(live.model) && !live.looping)
                {
                    if (!live.loopAt)
                    {
                        uint32_t length = LengthOf(live.model, static_cast<uint32_t>(live.intro));
                        if (!length && live.oneShot) length = 2000; // no such animation: show it a moment
                        live.loopAt = now + length;
                    }
                    if (live.loopAt <= now)
                    {
                        // The intro has played once: a one-shot model goes, a lasting one loops --
                        // the kit's loop, else Hold, else its intro over again.
                        live.looping = true;
                        if (live.oneShot)
                        {
                            if (!live.endAt || live.endAt > now) live.endAt = now;
                        }
                        else
                        {
                            uint32_t loop = static_cast<uint32_t>(live.intro);
                            if (live.def->anim >= 0) loop = static_cast<uint32_t>(live.def->anim);
                            else if (LengthOf(live.model, kHold)) loop = kHold;
                            if (loop != static_cast<uint32_t>(live.intro)) g_m2->play_animation(live.model, loop, 1.0f);
                        }
                    }
                }
                if (live.endAt && live.endAt <= now) BeginEnding(live, now);
                continue;
            }
            if (live.state == State::Ending && live.goneAt <= now) Release(live.model);
        }
        g_lives.erase(std::remove_if(g_lives.begin(), g_lives.end(),
                                     [](const Live& l) { return l.state == State::Ending && !l.model; }),
                      g_lives.end());

        // Missiles.
        for (Missile& m : g_missiles)
        {
            if (!m.model) continue;
            if (!g_m2->model_loaded(m.model) && now < m.arrives) continue;
            if (m.target) PointOf(m.target, m.def->destination >= 0 ? m.def->destination : 15, m.to);
            const float span = static_cast<float>(m.arrives - m.launched);
            float t = span > 0 ? static_cast<float>(now - m.launched) / span : 1.0f;
            if (t >= 1.0f)
            {
                Release(m.model);
                continue;
            }
            const Vec at{m.from.x + (m.to.x - m.from.x) * t, m.from.y + (m.to.y - m.from.y) * t,
                         m.from.z + (m.to.z - m.from.z) * t};
            const float dx = m.to.x - m.from.x, dy = m.to.y - m.from.y, dz = m.to.z - m.from.z;
            wxl_m2_placement p{};
            p.struct_size = sizeof p;
            p.position    = {at.x, at.y, at.z};
            p.yaw         = std::atan2(dy, dx);
            p.pitch       = -std::atan2(dz, std::sqrt(dx * dx + dy * dy));
            p.scale       = m.def->scale;
            g_m2->place_model(m.model, &p);
        }
        g_missiles.erase(std::remove_if(g_missiles.begin(), g_missiles.end(), [](const Missile& m) { return !m.model; }),
                         g_missiles.end());

        // Casts nothing refers to any more.
        for (auto it = g_casts.begin(); it != g_casts.end();)
        {
            const uint32_t serial = it->first;
            bool used = now - it->second.lastUse < 2000;
            for (const Live& l : g_lives) used = used || l.cast == serial;
            for (const Missile& m : g_missiles) used = used || m.cast == serial;
            for (const Timed& t : g_timed) used = used || t.cast == serial;
            for (const Emitter& em : g_emitters) used = used || em.cast == serial;
            for (const auto& [unit, s] : g_castOf) used = used || s == serial;
            for (const auto& [key, s] : g_auraOf) used = used || s == serial;
            it = used ? std::next(it) : g_casts.erase(it);
        }
    }

    void Clear()
    {
        for (Live& live : g_lives) Release(live.model);
        for (Missile& m : g_missiles) Release(m.model);
        g_lives.clear();
        g_missiles.clear();
        g_emitters.clear();
        g_glows.clear();
        g_timed.clear();
        g_casts.clear();
        g_castOf.clear();
        g_auraOf.clear();
    }

    // --- The cast's moments ---------------------------------------------------------------------

    void Launch(Cast& cast)
    {
        const uint64_t now = NowMs();
        Vec to = GroundOf(cast);
        uint64_t toUnit = cast.hits.empty() ? cast.target : cast.hits.front();
        Vec from;
        uint32_t flight = 0;
        for (const MissileDef& m : cast.def->missiles)
        {
            if (!PointOf(cast.caster, m.attachment, from)) continue;
            const Vec cast_off = Turned(m.castOffset, cast.facing);
            from = {from.x + cast_off.x, from.y + cast_off.y, from.z + cast_off.z};
            Vec dest = to;
            if (toUnit) PointOf(toUnit, m.destination >= 0 ? m.destination : 15, dest);
            const float dx = dest.x - from.x, dy = dest.y - from.y, dz = dest.z - from.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float speed    = m.speed > 0 ? m.speed : cast.def->speed;
            const uint32_t ms    = speed > 0 ? static_cast<uint32_t>(distance / speed * 1000.0f) : 0;
            if (ms > flight) flight = ms;
            if (!ms) continue;
            Missile missile;
            missile.cast     = cast.serial;
            missile.def      = &m;
            missile.from     = from;
            missile.to       = dest;
            missile.target   = toUnit;
            missile.launched = now;
            missile.arrives  = now + ms;
            missile.model    = g_m2->create_world_model(wxl_str{m.path.data(), static_cast<uint32_t>(m.path.size())});
            if (missile.model) g_missiles.push_back(missile);
        }
        if (!flight && cast.def->speed > 0)
        {
            // No missile model, but the spell travels: impacts wait for the flight all the same.
            Vec start;
            if (PositionOf(cast.caster, start))
            {
                Vec dest = to;
                if (toUnit) PositionOf(toUnit, dest);
                const float dx = dest.x - start.x, dy = dest.y - start.y, dz = dest.z - start.z;
                flight = static_cast<uint32_t>(std::sqrt(dx * dx + dy * dy + dz * dz) / cast.def->speed * 1000.0f);
            }
        }
        g_timed.push_back({now + flight, cast.serial, TravelEnd});
        g_timed.push_back({now + flight, cast.serial, Impact});
    }

    void ReadCastTargets(Cast& cast, const wxl_spell_cast_args& args)
    {
        if (args.target_guid) cast.target = args.target_guid;
        if (args.has_dest)
        {
            cast.hasDest = true;
            cast.dest    = ToVec(args.dest);
        }
        if (args.hits && args.hit_count)
        {
            cast.hits.clear();
            const uint8_t* bytes = static_cast<const uint8_t*>(args.hits);
            for (uint32_t i = 0; i < args.hit_count; ++i)
            {
                uint64_t guid = 0;
                std::memcpy(&guid, bytes + i * 8, 8);
                if (guid) cast.hits.push_back(guid);
            }
        }
    }

    /// The cast `caster` has in progress of `spell`, if any.
    Cast* InProgress(uint64_t caster, uint32_t spell)
    {
        auto it = g_castOf.find(caster);
        if (it == g_castOf.end()) return nullptr;
        auto c = g_casts.find(it->second);
        if (c == g_casts.end() || c->second.def->id != spell) return nullptr;
        return &c->second;
    }

    class Casts final : public wxl::ObjectScript
    {
    public:
        void OnCastStart(const wxl_spell_cast_args& args)
        {
            const SpellDef* def = Def(args.spell_id);
            if (!def || !args.caster_guid) return;
            if (Cast* old = InProgress(args.caster_guid, args.spell_id)) EndAll(old->serial, true);
            Cast& cast = NewCast(def, args.caster_guid);
            ReadCastTargets(cast, args);
            g_castOf[args.caster_guid] = cast.serial;
            Fire(cast, PrecastStart);
        }

        void OnCastGo(const wxl_spell_cast_args& args)
        {
            const SpellDef* def = Def(args.spell_id);
            if (!def || !args.caster_guid) return;
            Cast* cast = InProgress(args.caster_guid, args.spell_id);
            if (!cast) cast = &NewCast(def, args.caster_guid);
            g_castOf.erase(args.caster_guid);
            ReadCastTargets(*cast, args);
            Fire(*cast, PrecastEnd);
            Fire(*cast, CastGo);
            Fire(*cast, TravelStart);
            Launch(*cast);
        }

        void Failed(uint64_t caster, uint32_t spell)
        {
            Cast* cast = InProgress(caster, spell);
            if (!cast) return;
            g_castOf.erase(caster);
            Fire(*cast, PrecastEnd);
            EndAll(cast->serial, true);
        }

        void OnSpellCastFailed(wxl::Object, wxl_guid caster, uint32_t spell, uint32_t, uint32_t, int32_t, int32_t) override
        {
            Failed(caster, spell);
        }
        void OnSpellCastInterrupted(wxl::Object, wxl_guid caster, uint32_t spell, uint32_t, uint32_t, int32_t, int32_t) override
        {
            Failed(caster, spell);
        }
        void OnSpellCastCancel(wxl::Object, wxl_guid caster, uint32_t spell, uint32_t, uint32_t, bool) override
        {
            Failed(caster, spell);
        }

        void OnSpellChannelStart(wxl::Object, wxl_guid caster_guid, wxl::Object, wxl_guid, uint32_t spell_id, uint32_t, uint32_t) override
        {
            const SpellDef* def = Def(spell_id);
            if (!def || !caster_guid) return;
            Cast* cast = nullptr;
            for (auto& [serial, c] : g_casts)
                if (c.caster == caster_guid && c.def == def && (!cast || c.born > cast->born)) cast = &c;
            if (!cast) cast = &NewCast(def, caster_guid);
            g_auraOf[{caster_guid, ~spell_id}] = cast->serial;
            Fire(*cast, ChannelStart);
        }

        void OnSpellChannelStop(wxl::Object, wxl_guid caster_guid, wxl::Object, wxl_guid, uint32_t spell_id, uint32_t, uint32_t) override
        {
            auto it = g_auraOf.find({caster_guid, ~spell_id});
            if (it == g_auraOf.end()) return;
            auto c = g_casts.find(it->second);
            g_auraOf.erase(it);
            if (c != g_casts.end()) Fire(c->second, ChannelEnd);
        }

        void OnUnitAuraChanged(wxl::Object unit, uint32_t spell_id, bool removed) override
        {
            const SpellDef* def = Def(spell_id);
            if (!def || (!def->Has(AuraStart) && !def->Has(AuraEnd))) return;
            const wxl::Expected<wxl_guid> guid = unit.Guid();
            if (!guid || !*guid) return;
            const AuraKey key{*guid, spell_id};
            auto it = g_auraOf.find(key);
            if (removed)
            {
                if (it == g_auraOf.end()) return;
                auto c = g_casts.find(it->second);
                g_auraOf.erase(it);
                if (c != g_casts.end()) Fire(c->second, AuraEnd);
                return;
            }
            if (it != g_auraOf.end()) return; // a refresh: what shows keeps showing
            // The aura's holder is the event's target; its caster, as far as the client knows, the
            // last caster of the spell on it, else the holder.
            Cast* from = nullptr;
            for (auto& [serial, c] : g_casts)
                if (c.def == def && (c.target == *guid || std::find(c.hits.begin(), c.hits.end(), *guid) != c.hits.end()) &&
                    (!from || c.born > from->born))
                    from = &c;
            Cast& cast    = NewCast(def, from ? from->caster : *guid);
            cast.target   = *guid;
            g_auraOf[key] = cast.serial;
            Fire(cast, AuraStart);
        }

        void OnObjectCreate(wxl::Object object, wxl_guid guid, wxl_object_type type) override
        {
            if (type != WXL_OBJECT_DYNAMIC) return;
            const wxl::DynamicObject area(object);
            const wxl::Expected<uint32_t> spell = area.SpellId();
            if (!spell) return;
            const SpellDef* def = Def(*spell);
            if (!def || (!def->Has(AreaStart) && !def->Has(AuraStart))) return;
            const wxl::Expected<wxl_guid> caster = area.CasterGuid();
            Cast& cast = NewCast(def, caster ? *caster : 0);
            const wxl::Expected<float> radius = area.Radius();
            if (radius) cast.radius = *radius;
            const wxl::Expected<wxl_vec3> at = area.Position();
            if (at)
            {
                cast.hasDest = true;
                cast.dest    = ToVec(*at);
            }
            g_auraOf[{guid, *spell}] = cast.serial;
            Fire(cast, AreaStart);
            // A ground aura's events (7 on the ground) start with the area too.
            for (const EventDef& e : def->events)
                if (e.start == AuraStart && e.target == OnGround)
                {
                    Fire(cast, AuraStart);
                    break;
                }
        }

        void OnObjectDestroy(wxl::Object, wxl_guid guid, wxl_object_type type) override
        {
            if (type != WXL_OBJECT_DYNAMIC) return;
            for (auto it = g_auraOf.begin(); it != g_auraOf.end(); ++it)
            {
                if (it->first.guid != guid) continue;
                auto c = g_casts.find(it->second);
                g_auraOf.erase(it);
                if (c != g_casts.end())
                {
                    Fire(c->second, AreaEnd);
                    Fire(c->second, AuraEnd);
                }
                return;
            }
        }
    };

    class World final : public wxl::WorldScript
    {
    public:
        void OnUpdate(float) override { Tick(); }
        void OnWorldEnter(uint32_t, wxl_vec3) override
        {
            if (!g_tableRead) ReadTable();
        }
        void OnWorldLeave(uint32_t, wxl_vec3) override { Clear(); }
    };

    Casts g_castScript;
    World g_worldScript;

    // Cast start and go read the whole struct (ReadCastTargets): raw subscriptions hand it over as
    // it was raised, where an override would take its 21 members one by one.
    wxl::Subscription g_castStart, g_castGo;

    void WXL_CALL OnCastStartRaw(void* user, const void* args)
    {
        static_cast<Casts*>(user)->OnCastStart(*static_cast<const wxl_spell_cast_args*>(args));
    }

    void WXL_CALL OnCastGoRaw(void* user, const void* args)
    {
        static_cast<Casts*>(user)->OnCastGo(*static_cast<const wxl_spell_cast_args*>(args));
    }

    class RetailSpellVisualsMod final : public wxl::Mod
    {
    public:
        wxl_result OnLoad() override
        {
            if (!g_m2 || !Carries(&wxl_m2_api::attachment_world))
            {
                wxl::Log(WXL_LOG_ERROR, kTag,
                         "wxl.m2 is missing or older than its world calls (create_world_model...): nothing to do");
                return WXL_E_UNSUPPORTED;
            }
            LoadSettings();
            glow::Init(&GlowsNow);
            wxl::ScriptMgr::Add(&g_castScript);
            wxl::ScriptMgr::Add(&g_worldScript);
            g_castStart = wxl::Subscribe(WXL_EV_SpellCastStart, &OnCastStartRaw, &g_castScript);
            g_castGo    = wxl::Subscribe(WXL_EV_SpellCastGo, &OnCastGoRaw, &g_castScript);
            wxl::Log(WXL_LOG_INFO, kTag, "playing retail spell visuals from DBFilesClient\\WXLRetailSpellVisuals.txt");
            return WXL_OK;
        }

        void OnShutdown() override
        {
            // The client is going: let go of nothing it frees itself.
            g_lives.clear();
            g_missiles.clear();
            g_glows.clear();
            glow::Shutdown();
        }

        void OnUnload() override
        {
            g_castStart = wxl::Subscription();
            g_castGo    = wxl::Subscription();
            glow::Shutdown();
            Clear();
        }
    };
}

WXL_MOD(RetailSpellVisualsMod, "wxl.retail-spellvisuals", "Retail spell visuals")
