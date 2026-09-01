#include "game/DualRing.h"

#include "game/Costume.h"
#include "ui/Grid.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace FUI::DualRing
{
    namespace
    {
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;

        // The forms we took kRing from, and owe it back to. Session state
        // only: a load restores every record from the plugin, so a debt
        // written into the cosave would already have been paid by the time we
        // read it back. Tick re-derives the arrangement instead (see header).
        std::vector<RE::FormID> g_stripped;

        // Where the player PUT the second ring -- which form is sitting in the
        // left cell. This is placement, not physics; see IsSecondCell in the
        // header for why it cannot be read off the slot bit. 0 means nobody
        // has placed one, and the doll falls back to the bit.
        RE::FormID    g_leftRing = 0;
        std::uint16_t g_leftSig  = 0;

        // The sweep walks the inventory, so it is not a per-frame job. It runs
        // when something asked it to and, while any bit is out, on a slow
        // heartbeat -- the world can change with no equip event we see.
        bool          g_dirty = false;
        std::uint64_t g_frame = 0;
        std::uint64_t g_nextSweep = 0;
        constexpr std::uint64_t kSweepGap = 30;   // ~0.5s at 60fps
        // A frame is 16.7ms at 60fps, so anything on this scale is a visible
        // hitch and should name itself in the log.
        constexpr double        kSweepWarnMs = 2.0;

        [[nodiscard]] const char* NameOf(RE::TESForm* a_f)
        {
            const char* n = a_f ? a_f->GetName() : nullptr;
            return (n && *n) ? n : "<unnamed>";
        }

        [[nodiscard]] bool HoldsRingBit(const RE::TESObjectARMO* a_armo)
        {
            return a_armo &&
                   (static_cast<std::uint32_t>(a_armo->GetSlotMask().get()) &
                    static_cast<std::uint32_t>(Slot::kRing)) != 0;
        }

        // Can this ring give up its slot bit and still count as a ring?
        //
        // Grid::IsRing answers yes on the kRing bit OR the ClothingRing
        // keyword. If we take the bit from a ring that has only the bit, it
        // stops being a ring to every reader in this plugin at once: the doll
        // files it as odd armour, the board's rules change under it, and --
        // the part that never recovers -- WornRings can no longer see it, so
        // the sweep never finds it to hand the bit back. That ring would be
        // stranded with no slot for the rest of the save.
        //
        // Every vanilla ring carries the keyword, so this refuses nothing an
        // ordinary game can produce. It exists for the modded ring parked on a
        // custom slot without the keyword, which has to be left alone.
        [[nodiscard]] bool MayGiveUpRingBit(const RE::TESObjectARMO* a_armo)
        {
            constexpr RE::FormID kClothingRing = 0x0010CD09;   // Skyrim.esm
            return a_armo && a_armo->HasKeywordID(kClothingRing);
        }

        void TakeRingBit(RE::TESObjectARMO* a_armo)
        {
            if (!a_armo) return;
            const auto mask = static_cast<std::uint32_t>(a_armo->GetSlotMask().get());
            a_armo->bipedModelData.bipedObjectSlots =
                static_cast<Slot>(mask & ~static_cast<std::uint32_t>(Slot::kRing));
            if (std::find(g_stripped.begin(), g_stripped.end(), a_armo->GetFormID()) ==
                g_stripped.end()) {
                g_stripped.push_back(a_armo->GetFormID());
            }
            SKSE::log::info("[DUALRING] '{}' gave up its ring slot", NameOf(a_armo));
        }

        // Only ever give a bit back to a form we took one from. A modded ring
        // can be authored with no kRing bit at all -- sitting on a custom slot
        // and identifying itself through the ClothingRing keyword -- and
        // handing that ring a bit it never had would move it onto the finger
        // its author deliberately kept clear.
        void GiveRingBitBack(RE::FormID a_id)
        {
            auto* f = RE::TESForm::LookupByID(a_id);
            auto* armo = f ? f->As<RE::TESObjectARMO>() : nullptr;
            if (!armo) return;
            const auto mask = static_cast<std::uint32_t>(armo->GetSlotMask().get());
            armo->bipedModelData.bipedObjectSlots =
                static_cast<Slot>(mask | static_cast<std::uint32_t>(Slot::kRing));
            SKSE::log::info("[DUALRING] '{}' has its ring slot back", NameOf(armo));
        }

        // One worn ring unit: the form and the list that names THIS unit.
        struct WornRing
        {
            RE::TESObjectARMO*  armo = nullptr;
            RE::ExtraDataList*  xl   = nullptr;
        };

        // Every ring the body is wearing, sorted by FormID so that "which one
        // keeps kRing" gives the same answer twice running. An arbitrary order
        // would move the visible ring from one finger to the other on every
        // load, for no reason the player could see.
        [[nodiscard]] std::vector<WornRing> WornRings(RE::PlayerCharacter* a_p)
        {
            std::vector<WornRing> out;
            if (!a_p) return out;
            for (const auto& [obj, data] : a_p->GetInventory(
                     [](RE::TESBoundObject& o) { return o.IsArmor(); })) {
                if (data.first <= 0 || !data.second) continue;
                auto* armo = obj->As<RE::TESObjectARMO>();
                if (!armo || Costume::IsAnchor(armo) || !Grid::IsRing(armo)) continue;
                if (!data.second->extraLists) continue;
                for (auto* xl : *data.second->extraLists) {
                    if (!xl || !(xl->HasType<RE::ExtraWorn>() ||
                                 xl->HasType<RE::ExtraWornLeft>())) {
                        continue;
                    }
                    out.push_back({ armo, xl });
                }
            }
            std::sort(out.begin(), out.end(), [](const WornRing& a, const WornRing& b) {
                return a.armo->GetFormID() < b.armo->GetFormID();
            });
            return out;
        }

        // One inventory walk per frame, shared -- the same trade-off the doll
        // already makes. GetInventory DEEP-COPIES every matching entry, which
        // is why CollectEquipment was rebuilt around a single shared walk;
        // asking again for each separate question quietly undid that saving. A
        // single ring swap used to ask four times (the drop gate, MakeRoom, the
        // cap check and AimAt) plus the sweep's own walk, all within one frame,
        // over every piece of armour the player owns.
        //
        // Only MEMBERSHIP is cached. Slot bits are still read live off the
        // ARMO, so a mask changed by this pass is visible to the next reader
        // immediately -- which is exactly what steps 3 and 4 of
        // PrepareForEquip depend on.
        std::vector<WornRing> g_wornCache;
        std::uint64_t         g_wornFrame = ~0ull;

        [[nodiscard]] const std::vector<WornRing>& WornRingsCached(RE::PlayerCharacter* a_p)
        {
            if (g_wornFrame != g_frame) {
                g_wornCache = WornRings(a_p);
                g_wornFrame = g_frame;
            }
            return g_wornCache;
        }

        // Membership changed underneath us, so the next reader must walk again.
        void ForgetWorn() { g_wornFrame = ~0ull; }

        // Remember which cell the player put a ring in (see the header).
        // Private, because the only moment anything knows this is while
        // PrepareForEquip is acting on the request that carries it.
        void NoteSecondCell(const RE::TESObjectARMO* a_ring, std::uint16_t a_sig)
        {
            g_leftRing = a_ring ? a_ring->GetFormID() : 0;
            g_leftSig  = a_ring ? a_sig : 0;
        }

        // The enchantment a worn unit actually carries: the unit's own when the
        // player enchanted it (ExtraEnchantment), otherwise the record's.
        // Both cases matter here. A pair of vanilla Rings of Resist Magic share
        // one formEnchanting exactly as a pair of player-enchanted rings share
        // one ExtraEnchantment, and the engine dispels either the same way.
        [[nodiscard]] RE::EnchantmentItem* EnchantmentOf(const RE::TESObjectARMO* a_armo,
                                                         RE::ExtraDataList* a_xl)
        {
            if (a_xl) {
                if (const auto* xe = a_xl->GetByType<RE::ExtraEnchantment>();
                    xe && xe->enchantment) {
                    return xe->enchantment;
                }
            }
            return a_armo ? a_armo->formEnchanting : nullptr;
        }

        // The invariant, restored by reading the body:
        //
        //     IF ANY RING IS WORN, EXACTLY ONE OF THEM HOLDS kRing.
        //
        // This pass originally enforced only "at most one", which turned out to
        // be half a rule. Take the visible ring off a pair and the survivor is
        // left with no bit at all: one ring on the body, wearing no slot,
        // invisible for the rest of the save with nothing to notice. Once both
        // directions are enforced, all the bookkeeping falls out of a single
        // question asked of the body -- there is no state to keep in step, and
        // every stale case repairs itself on the next sweep.
        //
        // Returns false when the body could not be read. The caller then stays
        // armed and asks again, rather than dropping the request on the floor.
        bool Enforce()
        {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!p || !p->Is3DLoaded()) return false;

            const auto& worn = WornRingsCached(p);

            // Counted over FORMS, not units. The slot bit lives on the ARMO, so
            // two plain rings of the same form share one bit. Counting units
            // made this pass see two holders where there was really one, strip
            // the shared form, then see NO holder on the next sweep and hand
            // the bit straight back -- a give/take oscillation twice a second,
            // which the player sees as a ring blinking in and out. The
            // invariant is about forms because the thing it governs is.
            std::vector<RE::TESObjectARMO*> forms;
            for (const auto& w : worn) {
                if (std::find(forms.begin(), forms.end(), w.armo) == forms.end()) {
                    forms.push_back(w.armo);
                }
            }

            // Forget the left cell's occupant the moment it comes off, so the
            // next ring to land there is not drawn in a cell it never took.
            // Same discipline as the slot bits: observed, not remembered.
            if (g_leftRing != 0 &&
                std::none_of(forms.begin(), forms.end(),
                    [](const RE::TESObjectARMO* a) { return a->GetFormID() == g_leftRing; })) {
                g_leftRing = 0;
                g_leftSig  = 0;
            }

            // 1. Hand back every bit whose form has left the body entirely.
            for (auto it = g_stripped.begin(); it != g_stripped.end();) {
                const bool stillWorn = std::any_of(forms.begin(), forms.end(),
                    [&](const RE::TESObjectARMO* a) { return a->GetFormID() == *it; });
                if (stillWorn) { ++it; continue; }
                GiveRingBitBack(*it);
                it = g_stripped.erase(it);
            }
            if (forms.empty()) return true;

            const auto holders = std::count_if(forms.begin(), forms.end(),
                [](const RE::TESObjectARMO* a) { return HoldsRingBit(a); });

            // 2. Nobody holds the bit, so the ring that did has come off. Give
            // it back to one of the survivors -- but only to a ring WE took it
            // from, never to one authored without it, which would move that
            // ring onto a finger its author deliberately kept clear.
            if (holders == 0) {
                for (auto* armo : forms) {
                    // Never give the bit to a form that has two units worn.
                    // They share the one bit, so handing it over arms the
                    // engine against BOTH of them: the next equip single-ends
                    // the slot and removes a ring the player never asked to
                    // take off. A doubled form stays slotless, which is the
                    // only state it can hold honestly.
                    const auto units = std::count_if(worn.begin(), worn.end(),
                        [&](const WornRing& w) { return w.armo == armo; });
                    if (units > 1) continue;
                    const auto id = armo->GetFormID();
                    const auto it = std::find(g_stripped.begin(), g_stripped.end(), id);
                    if (it == g_stripped.end()) continue;
                    GiveRingBitBack(id);
                    g_stripped.erase(it);
                    return true;
                }
                return true;
            }

            // 3. More than one holds it -- a load put the pair back into
            // contest. The FIRST keeps the slot; the rest give it up.
            if (holders <= 1) return true;
            bool kept = false;
            for (auto* armo : forms) {
                if (!HoldsRingBit(armo)) continue;
                if (!kept) { kept = true; continue; }
                // A ring that is not allowed to give its bit up keeps it, which
                // leaves two forms holding kRing after all. That is still the
                // better outcome: the alternative strands the ring with no slot
                // (see MayGiveUpRingBit), and the engine only re-reads the
                // contest at the next equip anyway.
                if (MayGiveUpRingBit(armo)) TakeRingBit(armo);
            }
            return true;
        }
    }

    bool HoldsRingSlot(const RE::TESObjectARMO* a_armo)
    {
        return HoldsRingBit(a_armo);
    }

    bool IsSecondCell(const RE::TESObjectARMO* a_armo, std::uint16_t a_sig)
    {
        return g_leftRing != 0 && a_armo &&
               a_armo->GetFormID() == g_leftRing && a_sig == g_leftSig;
    }

    bool HasSecondCell() { return g_leftRing != 0; }

    void RemoveWornUnit(RE::TESObjectARMO* a_armo, RE::ExtraDataList* a_xl)
    {
        auto* p  = RE::PlayerCharacter::GetSingleton();
        auto* em = RE::ActorEquipManager::GetSingleton();
        if (!p || !em || !a_armo) return;

        // Read this BEFORE the removal. The unequip rewrites the entry, and the
        // list we were handed can merge with an identical spare the moment
        // ExtraWorn comes off it.
        auto* leaving = EnchantmentOf(a_armo, a_xl);

        em->UnequipObject(p, a_armo, a_xl, 1, nullptr, false, false, false, true);
        ForgetWorn();
        if (!leaving) return;   // nothing was dispelled, so nothing lost it

        // Anything still worn that shared the dispelled enchantment has lost
        // its effect as collateral damage. Copy the list out before touching
        // anything: the repair below re-equips, which rebuilds the very cache
        // we are walking -- and holding an iterator across a call that grows a
        // container is how this file crashed once already.
        std::vector<std::pair<RE::TESObjectARMO*, std::uint16_t>> hurt;
        for (const auto& w : WornRingsCached(p)) {
            if (EnchantmentOf(w.armo, w.xl) != leaving) continue;
            hurt.push_back({ w.armo, Grid::InstanceSigOf(w.xl) });
        }
        if (hurt.empty()) return;

        // All off, then all back on -- never one at a time. Unequipping the
        // second would dispel the enchantment again and take the first one's
        // freshly restored effect with it. (Today's cap allows only one
        // survivor, but getting the order right costs nothing and the cap is
        // not a law of nature.)
        for (const auto& w : WornRingsCached(p)) {
            if (EnchantmentOf(w.armo, w.xl) != leaving) continue;
            em->UnequipObject(p, w.armo, w.xl, 1, nullptr, false, false, false, true);
        }
        ForgetWorn();
        for (const auto& [armo, sig] : hurt) {
            // Resolve the list here rather than earlier: the list we held a
            // moment ago is not the one this unit lives in now.
            auto* back = Grid::ExtraForPool(Grid::LiveEntryOf(p, armo), 0, sig);
            em->EquipObject(p, armo, back, 1, nullptr, false, false, false, true);
            SKSE::log::info("[DUALRING] '{}' put back on -- its enchantment was "
                            "dispelled with the ring that left", NameOf(armo));
        }
        ForgetWorn();
    }

    void PrepareForEquip(RE::TESObjectARMO* a_incoming, std::uint16_t a_sig,
                         RE::ExtraDataList* a_aimed, bool a_secondCell)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p || !a_incoming) return;

        // 1. Who has to come off? These are two separate questions, and
        //    running them as one was a bug.
        std::vector<WornRing> victims;
        {
            const auto& worn = WornRingsCached(p);

            // 1a. The ring the player pointed at always comes off. A drop onto
            //     an occupied cell is a swap, and a swap that keeps the
            //     occupant is not a swap. This used to be conditional on being
            //     over the cap, so it only fired when a second ring happened to
            //     be worn -- with just one ring on, dropping another onto its
            //     cell added instead of replacing. (Reported in Korean as
            //     "스왑이 아니고 밀려서 장착됨": "it didn't swap, it got pushed
            //     aside and equipped".) The cap is about how many rings the
            //     body may hold; the aim is about which ring the player asked
            //     to remove. Neither question answers the other.
            //
            //     Matched on the UNIT via its worn list, never on the form --
            //     see the header for the phantom ring that a form match caused.
            if (a_aimed) {
                for (const auto& w : worn) {
                    if (w.xl != a_aimed) continue;
                    victims.push_back(w);
                    break;
                }
            }

            // 1b. Then apply the cap to whoever is left, leaving room for one
            //     more ring to arrive.
            int over = static_cast<int>(worn.size() - victims.size()) - 1;
            for (const auto& w : worn) {
                if (over <= 0) break;
                if (std::any_of(victims.begin(), victims.end(),
                        [&](const WornRing& v) { return v.xl == w.xl; })) {
                    continue;
                }
                if (!HoldsRingBit(w.armo)) continue;   // prefer the visible one
                victims.push_back(w);
                --over;
            }
            for (const auto& w : worn) {
                if (over <= 0) break;
                if (std::any_of(victims.begin(), victims.end(),
                        [&](const WornRing& v) { return v.xl == w.xl; })) {
                    continue;
                }
                victims.push_back(w);
                --over;
            }
        }

        // 2. Take them off ourselves, never through the engine's conflict pass.
        //    That pass removes every worn ring whose slot mask overlaps the
        //    incoming ring's, and the slot bit is a fact about the form, so it
        //    cannot be aimed at one particular ring. Three attempts to steer it
        //    produced three different failures: a phantom ring on the cursor,
        //    both rings coming off, and a third ring going on. The caller skips
        //    that pass entirely for rings.
        //
        //    Removal goes through RemoveWornUnit rather than a bare
        //    UnequipObject, because the engine dispels worn enchantments by
        //    enchantment rather than by unit -- a bare removal here would strip
        //    the magic from the identical ring the player is keeping (header).
        //
        //    Each victim is named by (form, signature) and re-resolved as we
        //    go, never by the list pointers collected above: the first removal
        //    re-equips a survivor, which rewrites the entry those later
        //    pointers live in. Two victims can only arise past a cap of two,
        //    which nothing reaches today -- but a stale ExtraDataList* is not a
        //    bug that waits politely for the case that produces it.
        std::vector<std::pair<RE::TESObjectARMO*, std::uint16_t>> leaving;
        leaving.reserve(victims.size());
        for (const auto& v : victims) {
            leaving.push_back({ v.armo, Grid::InstanceSigOf(v.xl) });
        }
        victims.clear();   // its pointers are about to stop meaning anything
        for (const auto& [armo, sig] : leaving) {
            RE::ExtraDataList* xl = nullptr;
            for (const auto& w : WornRingsCached(p)) {
                if (w.armo != armo || Grid::InstanceSigOf(w.xl) != sig) continue;
                xl = w.xl;
                break;
            }
            if (!xl) continue;   // it left on its own between then and now
            SKSE::log::info("[DUALRING] '{}' comes off to make room", NameOf(armo));
            RemoveWornUnit(armo, xl);
        }

        // 3. Whoever is staying gives up the slot, so that the engine has
        //    nothing to single-end when the incoming ring goes on.
        bool sharesWithSurvivor = false;
        for (const auto& w : WornRingsCached(p)) {
            if (w.armo == a_incoming) sharesWithSurvivor = true;
            if (!HoldsRingBit(w.armo) || !MayGiveUpRingBit(w.armo)) continue;
            TakeRingBit(w.armo);
        }

        // 4. ...and the incoming ring takes the slot, so something is drawn on
        //    the hand. Except when a survivor shares its form: one ARMO has one
        //    bit, so handing it over would arm the engine against the very ring
        //    we just decided to keep.
        if (!sharesWithSurvivor) {
            const auto id = a_incoming->GetFormID();
            const auto it = std::find(g_stripped.begin(), g_stripped.end(), id);
            if (it != g_stripped.end()) {
                GiveRingBitBack(id);
                g_stripped.erase(it);
            }
        }

        // 5. Record which cell the player put it in. Placement, not physics --
        //    see the header.
        if (a_secondCell) NoteSecondCell(a_incoming, a_sig);
        else if (IsSecondCell(a_incoming, a_sig)) NoteSecondCell(nullptr, 0);

        g_dirty = true;
    }

    void Tick()
    {
        ++g_frame;
        // Cheap when there is nothing to do, which is nearly always: no bit is
        // out on loan and nobody has asked. The heartbeat only runs while we
        // still owe one.
        if (!g_dirty && g_stripped.empty()) return;
        if (!g_dirty && g_frame < g_nextSweep) return;
        g_nextSweep = g_frame + kSweepGap;
        // Measured rather than assumed. A reporter saw the game stall for a few
        // seconds and asked whether we were the cause, and the log could not
        // answer: it records events, not the time between them, and from the
        // outside idle looks exactly like frozen. So the sweep times itself,
        // and anything slow enough to matter reports its own numbers. This
        // costs one clock read on the frames that actually sweep.
        const auto t0 = std::chrono::steady_clock::now();
        // Cleared only when the sweep actually managed to read the body. A load
        // reaches here before the actor has its 3D, and a request dropped at
        // that point would leave a loaded pair both holding kRing -- which the
        // next equip's conflict pass would resolve by taking BOTH rings off.
        const bool ran = Enforce();
        if (ran) g_dirty = false;
        const auto ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
        if (ms > kSweepWarnMs) {
            SKSE::log::warn("[DUALRING] sweep took {:.1f}ms ({} worn ring(s), "
                            "{} slot(s) out) -- this is us", ms,
                            g_wornCache.size(), g_stripped.size());
        }
    }

    void RevertGame()
    {
        // The records are about to be re-read from the plugin anyway. This is
        // for the case where they are not -- starting a new game in the same
        // session. It is cheap either way, and it leaves none of our forms
        // edited across a save boundary.
        for (const auto id : g_stripped) GiveRingBitBack(id);
        g_stripped.clear();
        g_leftRing = 0;
        // Leave the sweep ARMED rather than clearing it. This runs before every
        // load, and the save about to open may already have two rings on. The
        // engine hands both slot bits back along with the records, so the pair
        // arrives in contest with no debt recorded and nothing to notice --
        // the first sweep after the body exists is what separates them again.
        // The cost is one extra walk when starting a new game.
        g_dirty = true;
    }
}
