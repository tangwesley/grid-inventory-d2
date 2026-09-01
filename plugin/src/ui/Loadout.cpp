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
            // armour -- and, for a RING, the doll's second ring cell. No new
            // field and no cosave bump: armour has always ignored the LeftHand
            // equip slot on the way back out, so the flag was free to mean
            // "which of the two slots" for the one item type that has two.
            // (1.6) Both rings are worn by the engine now, so for a ring this
            // flag means PLACEMENT and nothing more: which cell the tile is
            // drawn in. It has to be recorded here because DualRing forgets a
            // cell the moment its ring leaves the body, and UnequipAll takes
            // both rings off before the new set goes on.
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

        // The first preset is a gift rather than a purchase. A player who has
        // never spent any gold still has somewhere to put a second set, so the
        // feature can show what it does on a new character instead of being a
        // priced-out row.
        //
        // This is written as an invariant rather than as a one-off at new-game,
        // which buys two things: it also tops up saves made before the gift
        // existed, and it survives the player deleting the tab. Deleting the
        // only preset empties it instead of removing the feature -- and instead
        // of leaving NextCost with zero presets to price against.
        //
        // NextCost counts PURCHASED presets, so the gift does not push the
        // first real purchase up a tier. Buying still starts at 5000.
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

        // What a SET carries in a hand: gear, and only gear.
        //
        // A scroll is not gear -- it is a consumable. A tab holding one would
        // reserve a unit off the board for a tab that is not currently being
        // worn, and would then try to re-equip a copy the player has since
        // spent. Presets carry the weapon and the torch; what is readied in a
        // hand beyond that is the player's business each time.
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

        // Is this entry a ring? A ring is the one item that can occupy either
        // of two cells, so it is the one entry EquipSet has to say a cell for.
        [[nodiscard]] RE::TESObjectARMO* RingOf(const Entry& a_e)
        {
            auto* armo = RE::TESForm::LookupByID<RE::TESObjectARMO>(a_e.id);
            return (armo && Grid::IsRing(armo)) ? armo : nullptr;
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
                // A costume anchor is worn but is not the player's gear -- it is
                // a placeholder this plugin equips so that a bare slot has an
                // appearance list to lend. Capturing it would write a phantom
                // helmet into the preset, and activating that preset would then
                // try to equip a form the player is not supposed to own.
                if (Costume::IsAnchor(obj)) continue;
                // GI53: a worn SHIELD was already captured by the left-hand
                // path above -- a second (form, right) entry duplicated it in
                // the preset: ReservedCount said 2, EquipSet equipped it twice,
                // and a spare copy of the same shield vanished from the board.
                if (seen.contains({ obj->GetFormID(), true })) continue;
                // A second ring no longer needs a separate scan. It used to:
                // the engine wore one ring while the doll's other cell held a
                // carrier standing in for a ring that stayed in the pack, so
                // this walk went straight past it and the bottom-right cell was
                // the one slot on the doll that a preset never owned. Since
                // 1.6.0 both rings are worn by the engine and both turn up here.
                //
                // What still has to be asked is WHICH CELL, because that is
                // placement rather than a fact about the body. DualRing forgets
                // a cell as soon as its ring comes off, and UnequipAll takes
                // every ring off before the next set goes on.
                if (auto* armo = obj->As<RE::TESObjectARMO>();
                    armo && Grid::IsRing(armo)) {
                    const std::uint16_t sig = Grid::InstanceSigOf(
                        Grid::WornExtraOf(Grid::LiveEntryOf(a_p, obj), 0));
                    add(obj, DualRing::IsSecondCell(armo, sig));
                    continue;
                }
                add(obj, false);
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
            // The second ring used to have to come off first, and through
            // DualRing: a carrier was worn in its place, so the armour loop
            // below would strip the carrier and leave DualRing still believing
            // a ring was on the second cell. Since 1.6.0 both rings are
            // ordinary worn armour, so the loop takes them off like anything
            // else -- and the slot bits DualRing borrowed are handed back by its
            // own sweep, which reads the body rather than trusting a flag.
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
            // One pass. This used to be two, because a second ring went on
            // through a carrier that could only stand in beside a first ring
            // that was already there. Both rings are worn by the engine now and
            // both go on the ordinary way, in the order the set records them.
            for (const auto& e : a_items) {
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
                // Announce a ring before it goes on. The slot bit is a fact
                // about the FORM, so a bare EquipObject for the second ring
                // makes the engine single-end the kRing slot and displace the
                // first -- a two-ring set would end up wearing one.
                // PrepareForEquip is the single call that takes the bit off
                // whoever is staying, hands it to the incoming ring and records
                // the cell; every other path a ring arrives on already goes
                // through it.
                //
                // No aimed list is passed, because a preset points at no
                // occupant. Nothing is being swapped either: UnequipAll has
                // already emptied the body, so the cap has room and takes no
                // victim.
                if (auto* ring = RingOf(e)) {
                    DualRing::PrepareForEquip(ring, e.sig, nullptr, e.leftHand);
                }
                a_em->EquipObject(a_p, obj, xl, 1, slot, false, false, true, true);
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

            // This guard is not a nicety -- it is the difference between a
            // working switch and a wiped tab. Stripping and re-dressing fires a
            // storm of equip events, and a re-read landing between the strip
            // and the re-dress would write "wearing nothing" over the tab
            // currently being built.
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
            // The costume points at a tab by INDEX, so a deletion either
            // destroys that reference or shifts it. Left alone, the costume
            // would silently dress the player as whichever tab slid into the
            // gap.
            Costume::OnTabRemoved(a_idx);
            // The quick wheel's arrangement needs the same notice, for the same
            // reason. It rebuilds itself every time it opens and needs no other
            // notification -- but a deletion is invisible to a rebuild, because
            // the index it frees is immediately taken by the tab above it.
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
        // A stackable has no particular "this one" to hold back, so reserving
        // from a stack just makes one unit stop existing on screen.
        //
        // The reservation does earn its keep for GEAR. A tab holding an iron
        // sword means one iron sword must not be sold out from under it, and
        // hiding that one from the board is how we say so. "One entry equips
        // one item" is true there.
        //
        // It is not true of arrows. A tab wearing a bow and a quiver holds "the
        // quiver", not one arrow -- and one arrow is exactly what this used to
        // subtract, out of a pool of hundreds, permanently. The board draws
        // (count minus this), see the stackable branch in Grid.cpp, so that
        // arrow was gone from the grid while the engine still had it:
        //
        //   240 in our UI / 241 in player.getitemcount   -- one tab with arrows
        //   258 in our UI / 260                          -- two tabs
        //   everything dropped: UI empty / getitemcount 1
        //
        // It was reported as arrows leaking, but it was never ammo-specific:
        // every stackable a preset ever wore was doing the same thing quietly,
        // torches most of all. (2026-08-31, confirmed by the player, whose
        // presets held arrows.)
        //
        // Nothing is left unprotected by returning 0 here. A stackable held by
        // an inactive tab that the player then spends is simply not there when
        // that tab is next worn, and EquipSet already skips whatever is
        // missing. That is the honest outcome, and the one vanilla gives.
        if (auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(a_id);
            obj && Grid::StackCap(obj) > 1) {
            return 0;
        }
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

        // Re-read here, on the game thread, and not inside FormsOf. FormsOf is
        // called from the render pass -- the quick menu draws ten slots a frame
        // through it -- and a re-read means an inventory scan. Scanning the
        // player's inventory from the render thread, while the game thread is
        // free to change it, is a crash waiting for a busy frame. This runs
        // every tick, so a stale reading never survives longer than one.
        //
        // It also collapses a change into a single scan rather than one per
        // event: switching a whole set fires an equip event per piece, and they
        // all fold into this one re-read.
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
