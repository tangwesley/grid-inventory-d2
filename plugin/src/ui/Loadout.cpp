#include "ui/Loadout.h"
#include "game/Costume.h"
#include "game/DualRing.h"
#include "ui/Equip.h"
#include "ui/Grid.h"
#include "ui/Lang.h"
#include "ui/Wheeler.h"

#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace FUI::Loadout
{
    namespace
    {
        struct Entry
        {
            RE::FormID    id = 0;
            // The LEFT side of the doll, whatever that side means for the item:
            // the off hand for a weapon or torch, the shield's own slot for
            // armour -- and, for a RING, the doll's second ring slot, which is
            // worn through DualRing's carrier rather than by the engine (see
            // IsRing2). No new field and no cosave bump: armour has always
            // ignored the LeftHand equip slot on the way back out, so the flag
            // was free to mean "which of the two slots" for the one item type
            // that has two.
            bool          leftHand = false;
            // D4-b: WHICH unit, not just which form. A preset that remembers
            // only the FormID cannot tell the tempered dagger from the plain
            // one, so switching tabs silently swapped them. 0 = no signature
            // (a plain unit), which still means "engine's choice".
            std::uint16_t sig = 0;
        };
        struct LO
        {
            std::string        name;
            std::vector<Entry> items;
        };

        std::vector<LO> g_loadouts;
        int             g_active = 0;

        int  g_pendingSwitch = -1;
        bool g_pendingPurchase = false;
        int  g_pendingRemove = -1;

        // The active tab's list versus the gear actually on the player. Set by
        // the equip event, cleared by the re-read it triggers.
        bool g_activeStale = false;
        bool g_switching = false;   // DoSwitch is mid-strip; do not re-read

        constexpr RE::FormID kLeftHandSlot = 0x13F43;   // BGSEquipSlot "LeftHand"
        constexpr RE::FormID kGold001 = 0x0000000F;
        constexpr int        kCostStep = 5000;

        // Default name for a new preset: the smallest unused "<Preset> N", so
        // delete -> rebuy never produces two tabs with the same label.
        std::string NextPresetName()
        {
            auto nameOf = [](int v) {
                return std::string(Lang::T(Lang::Str::Preset)) + " " + std::to_string(v);
            };
            auto used = [&](int v) {
                for (const auto& lo : g_loadouts) {
                    if (lo.name == nameOf(v)) return true;
                }
                return false;
            };
            int n = 1;
            while (used(n)) ++n;
            return nameOf(n);
        }

        // ★THE FIRST PRESET IS A GIFT, not a purchase. A player who has never
        // spent gold still has somewhere to put a second set, so the feature
        // shows what it is on a new character instead of being a priced-out
        // row. Written as an INVARIANT rather than a one-off at new-game: it
        // also tops up saves made before the gift existed, and it survives the
        // player deleting the tab -- deleting the only preset empties it
        // instead of taking the feature away (and instead of leaving
        // NextCost with zero presets to price against).
        // ★NextCost counts PURCHASED presets, so the gift does not push the
        // first real purchase up a tier: buying still starts at 5000.
        void EnsureFreePreset()
        {
            if (g_loadouts.size() >= 2) return;
            g_loadouts.push_back({ NextPresetName(), {} });
        }

        void EnsureInit()
        {
            if (!g_loadouts.empty()) return;
            g_loadouts.push_back({ "EQUIP", {} });   // Name(0) is localised via Lang
            g_active = 0;
            EnsureFreePreset();
        }

        // A two-hander is reported by the engine in BOTH hands; two copies of one
        // one-hander are too. Telling them apart by comparing the FORM pointers
        // calls a legitimate dual wield "the same item", which is what made the
        // preset record only the right hand.
        bool BothHandsAreOneItem(RE::TESForm* a_right, RE::TESForm* a_left)
        {
            if (a_right != a_left || !a_right) return false;
            if (auto* w = a_right->As<RE::TESObjectWEAP>()) {
                using WT = RE::WEAPON_TYPE;
                const auto t = w->GetWeaponType();
                return t == WT::kTwoHandSword || t == WT::kTwoHandAxe ||
                       t == WT::kBow || t == WT::kCrossbow || t == WT::kStaff;
            }
            return true;   // not a weapon and reported in both hands
        }

        // What a SET carries in a hand: gear, and only gear. ★A SCROLL IS NOT
        // GEAR -- it is a consumable, and a tab holding one would reserve a
        // unit off the board for a tab that is not being worn and then try to
        // re-equip a copy the player has since spent. Presets carry the
        // weapon and the torch; what is readied in a hand is the player's
        // business each time.
        bool IsSetHandItem(RE::TESForm* a_f)
        {
            return a_f && (a_f->Is(RE::FormType::Weapon) || a_f->Is(RE::FormType::Light));
        }

        // What the STRIP has to take off, which is wider on purpose: everything
        // the doll can show in a hand. A scroll being NO tab's property is
        // exactly why it must come off -- left standing it rides into the next
        // preset, filling a hand that preset never filled itself.
        bool IsWornHandItem(RE::TESForm* a_f)
        {
            return IsSetHandItem(a_f) || (a_f && a_f->Is(RE::FormType::Scroll));
        }

        // A ring recorded on the doll's second (left) ring slot. The engine
        // wears exactly one ring; this one is worn by DualRing's carrier while
        // the ring itself sits in the pack -- so it goes on and comes off
        // through DualRing, and never through EquipObject.
        bool IsRing2(const Entry& a_e)
        {
            return a_e.leftHand &&
                   Grid::IsRing(RE::TESForm::LookupByID<RE::TESObjectARMO>(a_e.id));
        }

        std::vector<Entry> CaptureWorn(RE::PlayerCharacter* a_p)
        {
            std::vector<Entry> out;
            // Keyed by form AND hand. Form alone threw the left-hand copy away
            // whenever the same weapon was worn in both hands, so switching tabs
            // and back came home one dagger short.
            std::set<std::pair<RE::FormID, bool>> seen;
            auto add = [&](RE::TESForm* f, bool left) {
                if (!f) return;
                auto* b = f->As<RE::TESBoundObject>();
                if (!b) return;
                if (!seen.insert({ b->GetFormID(), left }).second) return;
                // ...and read the signature from THAT hand's list, not "the first
                // worn list of this form" -- with a tempered copy in one hand and
                // a plain one in the other, both slots recorded the same unit.
                // GI54: a SHIELD is armour (ExtraWorn, hand-free) -- reading
                // hand 2 missed its list and recorded sig 0 for tempered shields.
                const int readHand = left ? (b->Is(RE::FormType::Armor) ? 0 : 2) : 1;
                const std::uint16_t sig = Grid::InstanceSigOf(
                    Grid::WornExtraOf(Grid::LiveEntryOf(a_p, b), readHand));
                out.push_back({ b->GetFormID(), left, sig });
            };

            auto* right = a_p->GetEquippedObject(false);
            auto* left = a_p->GetEquippedObject(true);
            const bool oneItem = BothHandsAreOneItem(right, left);
            if (IsSetHandItem(right)) add(right, false);
            if (left && !oneItem && (IsSetHandItem(left) || left->Is(RE::FormType::Armor))) {
                add(left, true);
            }
            if (auto* ammo = Equip::EquippedAmmo(a_p)) add(ammo, false);

            auto inv = a_p->GetInventory(
                [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Armor); });
            for (auto& [obj, data] : inv) {
                if (data.first <= 0 || !data.second || !data.second->IsWorn()) continue;
                // ★The costume anchor is worn but is not the player's gear -- it
                // is a placeholder this plugin equips to give a bare slot an
                // appearance list. Capturing it would write a phantom helmet
                // into the preset, and activating that preset would then try to
                // equip a form the player is not supposed to own.
                if (Costume::IsAnchor(obj)) continue;
                // GI53: a worn SHIELD was already captured by the left-hand
                // path above -- a second (form, right) entry duplicated it in
                // the preset: ReservedCount said 2, EquipSet equipped it twice,
                // and a spare copy of the same shield vanished from the board.
                if (seen.contains({ obj->GetFormID(), true })) continue;
                add(obj, false);
            }

            // ★★THE SECOND RING, which no scan above could have found. The
            // engine wears one ring; the doll's other ring slot is a CARRIER
            // standing in for a ring that stays in the pack (DualRing), so the
            // worn walk goes straight past it. The tab therefore captured one
            // ring and never the other, and the bottom-right slot kept whatever
            // it held through every switch -- the one slot on the doll that did
            // not belong to the preset.
            // ★AFTER the armour loop, never before: that loop skips a form
            // already seen on the left side (the shield rule), so an entry
            // planted first would have swallowed a FIRST ring of the same form.
            // It is also the order EquipSet needs -- see there.
            // ★The signature comes from DualRing, not from a worn list: the
            // ring is not worn, so there is no worn list to read it off.
            if (!DualRing::TakeOffPending()) {   // asked for off; do not record it
                if (auto* second = DualRing::Second()) {
                    if (seen.insert({ second->GetFormID(), true }).second) {
                        out.push_back({ second->GetFormID(), true, DualRing::SecondSig() });
                    }
                }
            }
            return out;
        }

        void UnequipAll(RE::PlayerCharacter* a_p, RE::ActorEquipManager* a_em)
        {
            // D4: name the WORN sub-stack. With a null list the engine chooses,
            // and with spares of the same form in the pack it does not have to
            // choose the one actually on the body -- which left the doll showing
            // an item the game considered unequipped.
            // ...and name the HAND too. With one dagger in each hand both calls
            // resolved to "the first worn list of this form", so the right-hand
            // unequip could strip the LEFT copy and leave the right one standing
            // -- which is why activating an empty preset left a dagger behind,
            // sitting in the weapon slot instead of the shield slot it came from.
            auto worn = [&](RE::TESBoundObject* b, int hand) {
                return Grid::WornExtraOf(Grid::LiveEntryOf(a_p, b), hand);
            };
            auto un = [&](RE::TESForm* f, int hand) {
                if (!f) return;
                if (auto* b = f->As<RE::TESBoundObject>()) {
                    const RE::BGSEquipSlot* slot =
                        hand == 2 ? RE::TESForm::LookupByID<RE::BGSEquipSlot>(kLeftHandSlot)
                                  : nullptr;
                    a_em->UnequipObject(a_p, b, worn(b, hand), 1, slot, false, false, true, true);
                }
            };
            // ★★The second ring FIRST, and through DualRing rather than as
            // ordinary armour. The carrier is worn, so the armour loop below
            // would happily unequip it -- and leave DualRing still believing a
            // ring is on the second slot: the effect gone, the ring still off
            // the board, the doll still drawing it. TakeOff is the only thing
            // that stands the whole arrangement down together.
            // ★And withdraw any take-off the render pass queued. It would
            // otherwise run in DualRing::Tick -- which comes AFTER
            // ProcessPending in the same tick -- and strip the second ring the
            // incoming preset had just put on.
            if (DualRing::Second()) DualRing::TakeOff();
            DualRing::CancelTakeOff();

            auto* right = a_p->GetEquippedObject(false);
            auto* left = a_p->GetEquippedObject(true);
            const bool oneItem = BothHandsAreOneItem(right, left);
            if (IsWornHandItem(right)) un(right, 1);
            if (left && !oneItem && IsWornHandItem(left)) un(left, 2);
            if (auto* ammo = Equip::EquippedAmmo(a_p)) {
                a_em->UnequipObject(a_p, ammo, worn(ammo, 0), 1, nullptr, false, false, true, true);
            }
            auto inv = a_p->GetInventory(
                [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Armor); });
            for (auto& [obj, data] : inv) {
                if (data.first > 0 && data.second && data.second->IsWorn()) {
                    a_em->UnequipObject(a_p, obj, worn(obj, 0), 1, nullptr, false, false, true, true);
                }
            }
        }

        // Direct Gold001 count. RE::Actor::GetGoldAmount is OFF-LIMITS in this
        // plugin: on 1.6.1170 its BGSDefaultObjectManager::GetObject(DefaultObjectID)
        // lookup dereferences a garbage slot (0x160000) and CTDs — proven by the
        // symbolized "+"-click crash chain (PlayerGold -> GetGoldAmount).
        int GoldCount(RE::PlayerCharacter* a_p)
        {
            auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(kGold001);
            if (!gold) return 0;
            int cnt = 0;
            auto inv = a_p->GetInventory([&](RE::TESBoundObject& o) { return &o == gold; });
            for (auto& [o, d] : inv) cnt = d.first;
            return cnt;
        }

        bool StillOwned(RE::PlayerCharacter* a_p, RE::TESBoundObject* a_obj)
        {
            int cnt = 0;
            auto inv = a_p->GetInventory([&](RE::TESBoundObject& o) { return &o == a_obj; });
            for (auto& [o, d] : inv) cnt = d.first;
            return cnt > 0;
        }

        void EquipSet(RE::PlayerCharacter* a_p, RE::ActorEquipManager* a_em,
                      const std::vector<Entry>& a_items)
        {
            // ★★TWO PASSES, and the order is not a preference. DualRing fills
            // the FIRST ring slot when it finds that one empty -- "the second
            // cannot be filled alone" -- so a set carrying two rings would put
            // its second ring on the first slot and then have the first ring
            // displace it. The engine-worn gear goes on first; the carrier
            // stands in afterwards, next to a first ring that is already there.
            for (const auto& e : a_items) {
                if (IsRing2(e)) continue;
                auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(e.id);
                if (!obj || !StillOwned(a_p, obj)) continue;   // sold/dropped -> skip
                const RE::BGSEquipSlot* slot = nullptr;
                // GI54: leftHand is recorded for shields (they come off the
                // left-hand path) but the LeftHand EQUIP SLOT belongs to
                // weapons and torches only -- armour equips through its own
                // biped slot.
                if (e.leftHand && !obj->Is(RE::FormType::Armor)) {
                    slot = RE::TESForm::LookupByID<RE::BGSEquipSlot>(kLeftHandSlot);
                }
                // D4-b: name the unit the preset actually captured. Sig 0 (a
                // plain unit) resolves to nullptr and the engine picks, which
                // is right -- plain units are interchangeable.
                auto* xl = Grid::ExtraForPool(Grid::LiveEntryOf(a_p, obj), 0, e.sig);
                a_em->EquipObject(a_p, obj, xl, 1, slot, false, false, true, true);
            }
            for (const auto& e : a_items) {
                if (!IsRing2(e)) continue;
                auto* ring = RE::TESForm::LookupByID<RE::TESObjectARMO>(e.id);
                if (!ring || !StillOwned(a_p, ring)) continue;   // sold/dropped -> skip
                // Same unit rule as above: the signature names the copy the tab
                // captured, and 0 lets DualRing take the engine's choice.
                auto* xl = Grid::ExtraForPool(Grid::LiveEntryOf(a_p, ring), 0, e.sig);
                if (!DualRing::Wear(ring, xl)) {
                    // Wear says why in its own log line; this says whose set it
                    // was, which is the part that would otherwise be a mystery.
                    SKSE::log::info("[LOADOUT] second ring 0x{:08X} not restored", e.id);
                }
            }
        }

        void DoSwitch(int a_target)
        {
            EnsureInit();
            auto* p = RE::PlayerCharacter::GetSingleton();
            auto* em = RE::ActorEquipManager::GetSingleton();
            if (!p || !em || !p->Is3DLoaded()) return;
            if (a_target < 0 || a_target >= static_cast<int>(g_loadouts.size())) return;
            if (a_target == g_active) return;

            // ★The guard is not politeness -- it is the difference between a
            // working switch and a wiped tab. Stripping and re-dressing fires
            // a storm of equip events, and a re-read that landed between the
            // strip and the re-dress would write "wearing nothing" over the
            // tab being built.
            g_switching = true;
            g_loadouts[g_active].items = CaptureWorn(p);   // snapshot leaving tab
            UnequipAll(p, em);                             // strip
            EquipSet(p, em, g_loadouts[a_target].items);   // equip target (missing skipped)
            g_active = a_target;
            g_switching = false;
            g_activeStale = false;   // the new tab's list IS what is now worn

            if (auto* proc = p->GetActorRuntimeData().currentProcess) {
                proc->Update3DModel(p);                     // force biped refresh while paused
            }
            // The costume dresses whatever is worn, and what is worn just
            // changed -- including possibly becoming the costume's own tab.
            Costume::MarkDirty();
            Grid::RequestRebuild();
            SKSE::log::info("[LOADOUT] switched to [{}] '{}' ({} items)",
                a_target, g_loadouts[a_target].name, g_loadouts[a_target].items.size());
        }

        void DoPurchase()
        {
            EnsureInit();
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!p) return;
            if (AtCap()) {
                SKSE::log::info("[LOADOUT] purchase blocked: preset cap ({})", kMaxPresets);
                return;
            }
            const int cost = NextCost();   // tab-count based: deleting restores the tier
            if (GoldCount(p) < cost) {
                SKSE::log::info("[LOADOUT] purchase blocked: need {} gold", cost);
                return;
            }
            if (auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(kGold001)) {
                p->RemoveItem(gold, cost, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            }
            const int idx = static_cast<int>(g_loadouts.size());
            g_loadouts.push_back({ NextPresetName(), {} });
            SKSE::log::info("[LOADOUT] purchased preset [{}] for {} gold", idx, cost);
            DoSwitch(idx);   // switching to a fresh empty tab strips the character
        }

        void DoRemove(int a_idx)
        {
            EnsureInit();
            if (a_idx < 1 || a_idx >= static_cast<int>(g_loadouts.size())) return;
            if (a_idx == g_active) {
                DoSwitch(0);   // back to EQUIP; the deleted tab's gear becomes free
                // GI53: DoSwitch bails without switching when the player's 3D
                // is not loaded -- erasing anyway leaves g_active pointing at
                // (or past) the erased slot, and the next snapshot writes out
                // of bounds. Keep the tab; deleting again later works.
                if (g_active == a_idx) {
                    SKSE::log::info("[LOADOUT] remove aborted: switch-away failed");
                    return;
                }
            }
            g_loadouts.erase(g_loadouts.begin() + a_idx);
            if (g_active > a_idx) --g_active;
            // ★The costume points at a tab by INDEX, so a deletion either kills
            // it or shifts it. Left alone it would silently dress the player as
            // whichever tab slid into the gap.
            Costume::OnTabRemoved(a_idx);
            // ★...and so does the quick wheel's arrangement, for the same
            // reason. It rebuilds itself on every open and needs no other
            // notice -- but a deletion is invisible to a rebuild, because the
            // number it freed is immediately occupied by the tab above.
            Wheeler::OnTabRemoved(a_idx);
            EnsureFreePreset();   // the free tab always comes back: delete = empty it
            Grid::RequestRebuild();
            SKSE::log::info("[LOADOUT] removed preset [{}]", a_idx);
        }
    }

    int Count()
    {
        EnsureInit();
        return static_cast<int>(g_loadouts.size());
    }

    int Active()
    {
        EnsureInit();
        return g_active;
    }

    const char* Name(int a_index)
    {
        EnsureInit();
        if (a_index == 0) return Lang::T(Lang::Str::EquipTab);   // localised base label
        if (a_index < 0 || a_index >= static_cast<int>(g_loadouts.size())) return "?";
        return g_loadouts[a_index].name.c_str();
    }

    void SetName(int a_index, const char* a_name)
    {
        EnsureInit();
        if (a_index < 1 || a_index >= static_cast<int>(g_loadouts.size()) || !a_name) return;
        g_loadouts[a_index].name = a_name;
    }

    int NextCost()
    {
        EnsureInit();
        // 5000 * (PURCHASED preset tabs + 1): [0] is EQUIP and [1] is the free
        // preset, so size() - 1 is the count the price is meant to climb with.
        return kCostStep * (static_cast<int>(g_loadouts.size()) - 1);
    }

    bool AtCap()
    {
        EnsureInit();
        return static_cast<int>(g_loadouts.size()) - 1 >= kMaxPresets;
    }

    int PlayerGold()
    {
        // Grid's cached gold (refreshed on Rebuild) — safe in the render pass;
        // the engine-side GetGoldAmount helper crashes on this runtime.
        return Grid::GoldAmount();
    }

    std::vector<std::uint16_t> ReservedSigs(RE::FormID a_id)
    {
        EnsureInit();
        std::vector<std::uint16_t> out;
        for (int i = 0; i < static_cast<int>(g_loadouts.size()); ++i) {
            if (i == g_active) continue;
            for (const auto& e : g_loadouts[i].items) {
                if (e.id == a_id) out.push_back(e.sig);
            }
        }
        return out;
    }

    int ReservedCount(RE::FormID a_id)
    {
        EnsureInit();
        int n = 0;
        for (int i = 0; i < static_cast<int>(g_loadouts.size()); ++i) {
            if (i == g_active) continue;   // active gear is equipped (worn check handles it)
            for (const auto& e : g_loadouts[i].items) {
                if (e.id == a_id) ++n;   // one entry equips one item
            }
        }
        return n;
    }

    bool IsReserved(RE::FormID a_id) { return ReservedCount(a_id) > 0; }

    void MarkActiveStale() { g_activeStale = true; }

    std::vector<RE::FormID> FormsOf(int a_index)
    {
        EnsureInit();
        std::vector<RE::FormID> out;
        if (a_index < 0 || a_index >= static_cast<int>(g_loadouts.size())) return out;
        out.reserve(g_loadouts[a_index].items.size());
        for (const auto& e : g_loadouts[a_index].items) out.push_back(e.id);
        return out;
    }

    void RequestSwitch(int a_target) { g_pendingSwitch = a_target; }
    void RequestPurchase() { g_pendingPurchase = true; }
    void RequestRemove(int a_index) { g_pendingRemove = a_index; }

    void ProcessPending()
    {
        if (g_pendingPurchase) { g_pendingPurchase = false; DoPurchase(); }
        if (g_pendingRemove >= 1) { const int r = g_pendingRemove; g_pendingRemove = -1; DoRemove(r); }
        if (g_pendingSwitch >= 0) {
            // P2: DoSwitch bails while the player 3D isn't loaded — consuming
            // the request first silently LOST the switch. Keep it queued until
            // the 3D is ready.
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (p && p->Is3DLoaded()) {
                const int t = g_pendingSwitch;
                g_pendingSwitch = -1;
                DoSwitch(t);
            }
        }

        // ★★Re-read HERE, on the game thread, and not inside FormsOf. FormsOf
        // is called from the render pass -- the quick menu draws ten slots a
        // frame through it -- and a re-read is an inventory scan. Scanning the
        // player's inventory from the render thread while the game thread is
        // free to change it is a crash waiting for a busy frame. This runs
        // every tick, so "stale" never outlives one.
        // ★Once per change, not once per event: a full set change fires an
        // equip event per piece, and they collapse into this single scan.
        if (g_activeStale && !g_switching) {
            auto* p = RE::PlayerCharacter::GetSingleton();
            // Main menu / mid-load: no player, or no body to read gear off.
            // Stay stale -- the tick comes round again.
            if (p && p->Is3DLoaded() && g_active >= 0 &&
                g_active < static_cast<int>(g_loadouts.size())) {
                g_loadouts[g_active].items = CaptureWorn(p);
                g_activeStale = false;
            }
        }
    }

    void ResetSession()
    {
        g_loadouts.clear();
        g_active = 0;
        g_pendingSwitch = -1;
        g_pendingPurchase = false;
        g_pendingRemove = -1;
        EnsureInit();
    }

    // ---- L3: cosave persistence ----
    // Record 'LODT' v1: [u32 active][u32 loadoutCount] then per loadout
    // [u32 nameLen][name bytes][u32 itemCount] { [u32 formID][u8 leftHand] }.
    // FormIDs go through ResolveFormID on load — items from removed plugins
    // are silently dropped (matches EquipSet's sold/dropped skip).

    namespace
    {
        constexpr std::uint32_t kVersion = 2;   // v2 adds Entry::sig
        constexpr std::uint32_t kMaxLoadouts = 64;
        constexpr std::uint32_t kMaxName = 256;
        constexpr std::uint32_t kMaxItems = 4096;
    }

    void SaveGame(SKSE::SerializationInterface* a_intfc)
    {
        EnsureInit();
        if (!a_intfc->OpenRecord(kRecordType, kVersion)) {
            SKSE::log::error("[LOADOUT] cosave: OpenRecord failed");
            return;
        }
        auto w32 = [&](std::uint32_t v) { a_intfc->WriteRecordData(v); };
        w32(static_cast<std::uint32_t>(g_active));
        w32(static_cast<std::uint32_t>(g_loadouts.size()));
        for (const auto& lo : g_loadouts) {
            w32(static_cast<std::uint32_t>(lo.name.size()));
            a_intfc->WriteRecordData(lo.name.data(),
                static_cast<std::uint32_t>(lo.name.size()));
            w32(static_cast<std::uint32_t>(lo.items.size()));
            for (const auto& e : lo.items) {
                w32(e.id);
                const std::uint8_t lh = e.leftHand ? 1 : 0;
                a_intfc->WriteRecordData(lh);
                a_intfc->WriteRecordData(e.sig);   // v2
            }
        }
        SKSE::log::info("[LOADOUT] cosave: saved {} tabs (active {})",
            g_loadouts.size(), g_active);
    }

    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        // v1 records carry no signature: load them with sig 0, which behaves
        // exactly as v1 did (engine picks the copy). Refusing them outright
        // would silently wipe every preset the player had.
        if (a_version < 1 || a_version > kVersion) {
            SKSE::log::warn("[LOADOUT] cosave: unsupported record v{} (max v{}) - skipped",
                            a_version, kVersion);
            return;
        }

        std::uint32_t active = 0, count = 0;
        if (!a_intfc->ReadRecordData(active) || !a_intfc->ReadRecordData(count)) return;
        count = (std::min)(count, kMaxLoadouts);

        std::vector<LO> in;
        in.reserve(count);
        bool ok = true;
        for (std::uint32_t i = 0; ok && i < count; ++i) {
            std::uint32_t nameLen = 0;
            if (!a_intfc->ReadRecordData(nameLen) || nameLen > kMaxName) { ok = false; break; }
            std::string name(nameLen, '\0');
            if (nameLen && !a_intfc->ReadRecordData(name.data(), nameLen)) { ok = false; break; }

            std::uint32_t items = 0;
            if (!a_intfc->ReadRecordData(items) || items > kMaxItems) { ok = false; break; }
            LO lo;
            lo.name = std::move(name);
            lo.items.reserve(items);
            for (std::uint32_t j = 0; j < items; ++j) {
                std::uint32_t id = 0;
                std::uint8_t  lh = 0;
                if (!a_intfc->ReadRecordData(id) || !a_intfc->ReadRecordData(lh)) {
                    ok = false;
                    break;
                }
                std::uint16_t sig = 0;
                if (a_version >= 2 && !a_intfc->ReadRecordData(sig)) { ok = false; break; }
                RE::FormID resolved = 0;
                if (a_intfc->ResolveFormID(id, resolved)) {   // plugin removed -> drop
                    lo.items.push_back({ resolved, lh != 0, sig });
                }
            }
            in.push_back(std::move(lo));
        }
        if (!ok || in.empty()) {
            SKSE::log::error("[LOADOUT] cosave: truncated record — keeping base");
            ResetSession();
            return;
        }
        g_loadouts = std::move(in);
        // A save written before the free preset existed comes back one tab
        // short -- the gift is granted to those characters too.
        EnsureFreePreset();
        g_active = (std::min)(static_cast<int>(active),
            static_cast<int>(g_loadouts.size()) - 1);
        SKSE::log::info("[LOADOUT] cosave: loaded {} tabs (active {})",
            g_loadouts.size(), g_active);
    }

    void RevertGame(SKSE::SerializationInterface*)
    {
        // fires before every load and on new game — the clean-slate hook
        ResetSession();
    }
}
