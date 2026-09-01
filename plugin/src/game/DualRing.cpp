#include "game/DualRing.h"

#include "game/Costume.h"
#include "game/WornLedger.h"
#include "ui/Grid.h"

#include <vector>

namespace FUI::DualRing
{
    namespace
    {
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
        using BO   = RE::BIPED_OBJECTS::BIPED_OBJECT;

        constexpr std::uint32_t kSlots = BO::kEditorTotal;   // 32

        // ★Costume anchor 32. Costume::kAnchorCount is 31 precisely so this one
        // is never raised OR dropped by that system: `need` there is
        // `j < groups.size()` and groups can hold at most 31 entries, so the
        // costume could never want it -- but its loop would still drop one it
        // found held, which is why the count was lowered rather than shared.
        constexpr std::uint32_t kCarrierId = 0x84A;
        constexpr const char*   kPlugin    = "Grid Inventory.esp";

        RE::FormID    g_ringId  = 0;   // the ring the carrier stands in for
        std::uint16_t g_ringSig = 0;   // its content signature (see SecondSig)
        // ★Its ExtraUniqueID -- the OTHER half of a unit name, and the half
        // ExtraForPool answers first. See SecondUid in the header for what
        // recording only the signature cost.
        std::uint16_t g_ringUid = 0;
        bool          g_wantOff = false;  // take-off asked for from the render pass

        // ★What the carrier borrowed, so it can be handed back. The carrier is
        // OUR form and no other actor wears it, so this is bookkeeping rather
        // than the safety problem it would be for a shared armour -- but a
        // stale enchantment would still follow it into its next use.
        struct Lent
        {
            RE::EnchantmentItem* ench = nullptr;
            bool                 held = false;
        };
        Lent g_lent;

        // ★★★...AND IT DOES NOT STAY NAKED. See the note inside Carrier() for
        // what the authored armature does. This is why the undressing is a
        // function instead of three lines inside the resolve:
        //
        // LOADING A SAVE PUTS THE CLOTHES BACK ON. The engine re-reads the
        // carrier's record from the plugin -- OnLoad has always known this,
        // which is the reason the enchantment has to be re-lent there -- and
        // armorAddons comes back with it. The strip used to live behind the
        // `if (!cached)` of the resolve, so it ran ONCE PER PROCESS: the first
        // load of a session was fine and every load after it wore a circlet's
        // armature again.
        //
        // Measured in a reporter's log: 1924 lines, two loads, one strip.
        // Now it is asked on every lend, and costs nothing when already bare.
        void StripCarrier(RE::TESObjectARMO* a_c)
        {
            if (!a_c || a_c->armorAddons.empty()) return;
            a_c->armorAddons.clear();
            SKSE::log::info("[DUALRING] carrier armature stripped (authored circlet addon)");
        }

        [[nodiscard]] RE::TESObjectARMO* Carrier()
        {
            // Not a function-local static initialiser: a miss must be
            // retryable. This can run before the data handler has the plugin,
            // and caching null there would disable the feature for the session.
            static RE::TESObjectARMO* cached = nullptr;
            if (!cached) {
                if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                    cached = dh->LookupForm<RE::TESObjectARMO>(kCarrierId, kPlugin);
                }
                // ★★★THE CARRIER MUST BE NAKED. It is a byte-for-byte clone of
                // the costume anchors, and the TEMPLATE carries a circlet's
                // whole wardrobe: BOD2 on the HAIR and circlet slots, a
                // circlet ARMA on its armature. The ordinary anchors never
                // show it -- the costume overwrites their armorAddons with
                // donor lists -- but the carrier is anchor 32 precisely so
                // the costume leaves it alone, which also left the authored
                // circlet addon LIVE. Wearing it therefore claimed the hair
                // slot and fought the helmet's addon: helmet invisible over a
                // bald head, on WHATEVER biped slot we parked the ARMO
                // (measured: 60 and 59 alike -- the ARMO slot was never the
                // actor, its armature was). Strip the armature once, here,
                // where the form is first resolved; the slot mask is
                // rewritten per wear by Lend already.
                StripCarrier(cached);
            }
            return cached;
        }

        [[nodiscard]] RE::TESObjectARMO* RingById(RE::FormID a_id)
        {
            if (!a_id) return nullptr;
            auto* f = RE::TESForm::LookupByID(a_id);
            return f ? f->As<RE::TESObjectARMO>() : nullptr;
        }

        [[nodiscard]] const char* NameOf(RE::TESForm* a_f)
        {
            const char* n = a_f ? a_f->GetName() : nullptr;
            return (n && *n) ? n : "<unnamed>";
        }

        // The ring the ENGINE is wearing on kRing, if any.
        [[nodiscard]] RE::TESObjectARMO* FirstRing(RE::PlayerCharacter* a_p)
        {
            if (!a_p) return nullptr;
            for (const auto& [obj, data] : a_p->GetInventory(
                     [](RE::TESBoundObject& o) { return o.IsArmor(); })) {
                if (data.first <= 0 || !data.second || !data.second->IsWorn()) continue;
                auto* a = obj->As<RE::TESObjectARMO>();
                if (!a || Costume::IsAnchor(a) || IsCarrier(a)) continue;
                if (Grid::IsRing(a)) return a;
            }
            return nullptr;
        }

        // ★★★AN ENCHANTMENT IS NOT ALWAYS ON THE FORM, and this whole file used
        // to assume it was.
        //
        // Every read here was `armo->formEnchanting`, which is where a VANILLA
        // ring keeps its effect. A player-enchanted ring does not: the engine
        // puts the created ENCH on the unit's own ExtraDataList, and item mods
        // that roll affixes per unit do the same -- Diablo In Skyrim explicitly
        // swaps an enchanted base for its plain template first, because
        // "ExtraEnchantment is inert when the record carries an EITM". So for
        // every such ring `formEnchanting` is NULL, and the consequences ran the
        // length of the feature:
        //
        //   Lend      handed the carrier a null enchantment, so the second slot
        //             wore an item that did nothing at all (user report).
        //   ShareAnEffect  saw two unenchanted rings and let any two affixed
        //             rings stack, which is the one thing this exists to stop.
        //
        // The list is the second half of an item's identity everywhere else in
        // this codebase; it is the second half here too.
        [[nodiscard]] RE::EnchantmentItem* EnchOn(RE::ExtraDataList* a_xl)
        {
            if (!a_xl) return nullptr;
            const auto* xe = a_xl->GetByType<RE::ExtraEnchantment>();
            return xe ? xe->enchantment : nullptr;
        }

        // The enchantment a ring is really carrying: the FORM's, else the one on
        // the unit named by (a_uid, a_sig).
        //
        // ★Resolved through the player's inventory rather than from a cached
        // list. ExtraDataList* must never be held across frames (the engine
        // reallocates and frees them), and an EnchantmentItem* is a form, which
        // is safe to keep -- so the walk happens here and only the form leaves.
        //
        // ★★BOTH HALVES OF THE NAME, because ExtraForPool answers them from
        // different branches and its signature branch REFUSES a uid-bearing
        // list outright (GI42). Asking with the signature alone therefore
        // resolves nothing at all in a load order where the engine hands out
        // uids -- see SecondUid in the header.
        //
        // ★An unnamed unit falls back to ANY unit of this form that carries an
        // enchantment. That is a guess, and it is the right one: the
        // alternative is no enchantment at all, which is the bug being fixed.
        // It is reached only by callers with no unit in hand -- the wheel's
        // slotless equip, and a record written before the name was saved.
        [[nodiscard]] RE::EnchantmentItem* RingEnch(RE::TESObjectARMO* a_ring,
                                                    std::uint16_t a_uid,
                                                    std::uint16_t a_sig)
        {
            if (!a_ring) return nullptr;
            if (a_ring->formEnchanting) return a_ring->formEnchanting;
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!p) return nullptr;
            RE::EnchantmentItem* found = nullptr;
            for (const auto& [obj, data] : p->GetInventory([&](RE::TESBoundObject& o) {
                     return &o == static_cast<RE::TESBoundObject*>(a_ring);
                 })) {
                auto* entry = data.second.get();
                // The unit the name points at -- the carried ring, which is a
                // PACK unit, so the pool resolver (worn lists excluded) is the
                // exact answer for it.
                found = EnchOn(Grid::ExtraForPool(entry, a_uid, a_sig));
                // ★...then the WORN unit, which is the exact answer for the
                // OTHER ring. SharesEffect asks about the ring on the engine's
                // own slot as often as about the carried one, and that one's
                // affix lives on the list the pool pass just excluded.
                if (!found) found = EnchOn(Grid::WornExtraOf(entry));
                // Last: any unit of this form that carries one. A guess, and
                // only for a caller that named no unit at all.
                if (!found && entry && entry->extraLists) {
                    for (auto* xl : *entry->extraLists) {
                        if (auto* e = EnchOn(xl)) { found = e; break; }
                    }
                }
                (void)obj;
                break;
            }
            return found;
        }

        // ★★"Same effect" is NOT "same enchantment form". Two rings of one
        // family at different strengths are separate forms carrying the same
        // EffectSetting, and stacking those is exactly what this feature exists
        // to prevent -- so the comparison is on the base effects.
        [[nodiscard]] bool ShareAnEffect(RE::EnchantmentItem* a_ex,
                                         RE::EnchantmentItem* a_ey)
        {
            if (!a_ex || !a_ey) return false;   // an unenchanted ring stacks with anything
            if (a_ex == a_ey) return true;
            for (auto* px : a_ex->effects) {
                if (!px || !px->baseEffect) continue;
                for (auto* py : a_ey->effects) {
                    if (py && py->baseEffect == px->baseEffect) return true;
                }
            }
            return false;
        }

        // The same question about two RINGS. The carried one is named by the
        // uid+sig the carry recorded; anything else has to be resolved without
        // a name (see RingEnch).
        struct UnitName
        {
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
        };
        [[nodiscard]] bool ShareAnEffect(RE::TESObjectARMO* a_x, RE::TESObjectARMO* a_y,
                                         UnitName a_nx = {}, UnitName a_ny = {})
        {
            if (!a_x || !a_y) return false;
            return ShareAnEffect(RingEnch(a_x, a_nx.uid, a_nx.sig),
                                 RingEnch(a_y, a_ny.uid, a_ny.sig));
        }

        [[nodiscard]] std::uint32_t WornMask(RE::PlayerCharacter* a_p)
        {
            std::uint32_t used = 0;
            for (const auto& [obj, data] : a_p->GetInventory(
                     [](RE::TESBoundObject& o) { return o.IsArmor(); })) {
                if (data.first <= 0 || !data.second || !data.second->IsWorn()) continue;
                if (auto* a = obj->As<RE::TESObjectARMO>()) {
                    used |= static_cast<std::uint32_t>(a->GetSlotMask().get());
                    // ★★THE ADDONS TOO. A modded helmet often draws through an
                    // ArmorAddon that covers slots its ARMO mask never names
                    // (hair physics on 60 is the classic). The ARMO mask alone
                    // called those slots free, the carrier sat down on one,
                    // and the engine culled the helmet's addon over the
                    // conflict: wear both rings, and the helmet turns
                    // invisible over a bald head (user report -- the preset
                    // cycling that "healed" it was removing the second ring).
                    for (auto* arma : a->armorAddons) {
                        if (arma) used |= static_cast<std::uint32_t>(arma->GetSlotMask().get());
                    }
                }
            }
            return used;
        }

        // A biped slot nothing is wearing right now.
        // ★"!ring2slot = N" (editor 44..60): the player's word on where the
        // carrier may sit. Slot habits are a MODLIST fact no mask can read --
        // the measurement below proves it -- so the escape hatch has to be an
        // ini line, not another heuristic.
        int g_slotOverride = -1;   // bit index; -1 = pick automatically

        // ★Searched from the TOP: the low custom slots (44-49) are where
        // cloaks, backpacks and lanterns live, so taking one of those picks a
        // fight with whatever the player already has installed. kFX01 (31) is
        // skipped as well -- it is the effect slot and builds no armour.
        [[nodiscard]] int FreeSlot(RE::PlayerCharacter* a_p)
        {
            const std::uint32_t used = WornMask(a_p);
            if (g_slotOverride >= 0 && !(used & (1u << g_slotOverride))) {
                return g_slotOverride;
            }
            for (int i = static_cast<int>(kSlots) - 3; i >= 14; --i) {
                // ★editor slots 50/51 (bits 20/21): the DECAPITATION slots.
                // Equipping anything there culls the head outright -- never a
                // valid parking spot however crowded the rest of the biped is.
                if (i == 20 || i == 21) continue;
                if (!(used & (1u << i))) return i;
            }
            return -1;
        }
        // ★★...and the scan now stops BELOW editor slot 60 (bit 30). Measured:
        // no worn ARMO or addon claimed 60, the carrier sat there, and the
        // helmet still went invisible over a bald head -- something in the
        // MODLIST watches that slot (hair-physics and helmet-toggle systems
        // are the usual tenants). A mask cannot see a watcher; the only
        // honest move is to stay out of the known bad neighbourhood and hand
        // the player the "!ring2slot" override for whatever their list does.

        // Lend an enchantment to the carrier and put it on a_mask.
        // ★A ring's enchantment is kConstantEffect (verified: no vanilla ARMO
        // carries EAMT at all), so there is no charge to manage -- lending the
        // form IS lending the effect. It must be in place BEFORE the equip,
        // because that is when the engine reads it.
        //
        // ★★IT TAKES THE ENCHANTMENT, NOT THE RING, and that is the whole fix.
        // It used to read `a_ring->formEnchanting` itself, which is null for
        // every per-unit enchantment (see RingEnch) -- so the carrier went on
        // bare and the second slot did nothing. The caller resolves; this only
        // lends what it is handed.
        //
        // ★A created ENCH is borrowed, not attached: nothing here touches an
        // ExtraDataList, so no reference is taken and none has to be released.
        // Reclaim puts the record's own back.
        void Lend(RE::TESObjectARMO* a_carrier, RE::EnchantmentItem* a_ench,
                  std::uint32_t a_mask)
        {
            if (!g_lent.held) {
                g_lent.ench = a_carrier->formEnchanting;
                g_lent.held = true;
            }
            // ★Every lend, not once a session -- a load puts the authored
            // armature back on the record. See StripCarrier.
            StripCarrier(a_carrier);
            a_carrier->formEnchanting = a_ench;
            a_carrier->bipedModelData.bipedObjectSlots = static_cast<Slot>(a_mask);
        }

        void Reclaim()
        {
            auto* c = Carrier();
            if (!c || !g_lent.held) return;
            c->formEnchanting = g_lent.ench;
            g_lent = {};
        }
    }

    void TakeOffImpl(bool a_standalone);   // defined below TakeOff

    void SetSlotOverride(int a_editorSlot)
    {
        // editor numbers run 30..61; bits run 0..31. Decapitation (50/51) and
        // FX01 (61) are refused even by hand -- they cull the head or build
        // no armour at all.
        const int bit = a_editorSlot - 30;
        const bool ok = bit >= 14 && bit <= 30 && bit != 20 && bit != 21;
        g_slotOverride = ok ? bit : -1;
        if (ok) {
            SKSE::log::info("[DUALRING] carrier slot pinned to {} (!ring2slot)",
                            a_editorSlot);
        }
    }

    int SlotOverride()
    {
        return g_slotOverride < 0 ? -1 : g_slotOverride + 30;
    }

    RE::TESObjectARMO* Second() { return RingById(g_ringId); }

    std::uint16_t SecondSig() { return g_ringSig; }

    std::uint16_t SecondUid() { return g_ringUid; }

    bool WouldDuplicate(RE::TESObjectARMO* a_ring)
    {
        auto* second = RingById(g_ringId);
        // ★The carried ring is named by the uid+sig the carry recorded -- the
        // one unit whose enchantment this question is actually about.
        return second && ShareAnEffect(second, a_ring, { g_ringUid, g_ringSig });
    }

    bool SharesEffect(RE::TESObjectARMO* a_x, RE::TESObjectARMO* a_y)
    {
        return ShareAnEffect(a_x, a_y);
    }

    bool IsCarrier(const RE::TESForm* a_form)
    {
        auto* c = Carrier();
        return c && a_form && a_form->GetFormID() == c->GetFormID();
    }

    Verdict CanWear(RE::TESObjectARMO* a_ring)
    {
        if (!a_ring || !Grid::IsRing(a_ring)) return Verdict::kNotARing;
        if (!Carrier()) return Verdict::kNoCarrier;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return Verdict::kNoCarrier;

        // ★★The OTHER ring, whichever slot it is on. Asking only about the
        // first slot made the drop rules asymmetric: dragging the LEFT ring
        // onto the right slot picks it up first, which empties the left slot,
        // and the drop was then refused for having no first ring -- so the
        // ring came off instead of moving. The right-to-left direction worked
        // only because that check happened to pass.
        auto* other = FirstRing(p);
        // ★Which UNIT `other` is, so the effect test below can name it. An
        // engine-worn ring is found by its worn list and needs no signature;
        // the CARRIED one is a pack unit among possible siblings, and only the
        // signature separates a per-unit affix from a plain twin.
        UnitName otherName{};
        if (!other) {
            other     = RingById(g_ringId);
            otherName = { g_ringUid, g_ringSig };
        }
        if (other == a_ring) {
            // ★FORM identity, not unit identity. A SPARE unit of the same form
            // is a second ring, and a plain pair of one form is legal (the
            // rule is the EFFECT, one test below) -- so refuse only when the
            // player owns a single unit, where "wear it beside itself" is the
            // only thing this drop could mean. The old form-level refusal
            // blocked plain pairs outright (user spec correction).
            int owned = 0;
            for (const auto& [obj2, data] : p->GetInventory(
                     [&](RE::TESBoundObject& o) { return &o == a_ring; })) {
                owned = data.first;
                (void)obj2;
            }
            if (owned <= 1) return Verdict::kAlreadyWorn;
        }
        if (ShareAnEffect(other, a_ring, otherName)) return Verdict::kSameEffect;
        if (FreeSlot(p) < 0) return Verdict::kNoFreeSlot;
        // ★An empty first slot is no longer a refusal. Where the ring lands is
        // Wear's decision -- it fills the first slot, or trades places with the
        // ring already on the second -- because a slot that takes a drag and
        // then drops the item on the floor is the worst of the options.
        return Verdict::kOk;
    }

    const char* VerdictText(Verdict a_v)
    {
        switch (a_v) {
        case Verdict::kNotARing:    return "not a ring";
        case Verdict::kAlreadyWorn: return "already worn";
        case Verdict::kSameEffect:  return "the same effect is already worn";
        case Verdict::kNoCarrier:   return "carrier form missing (is the esp loaded?)";
        case Verdict::kNoFreeSlot:  return "no free biped slot";
        default:                    return "";
        }
    }

    bool Wear(RE::TESObjectARMO* a_ring, RE::ExtraDataList* a_xl)
    {
        const auto v = CanWear(a_ring);
        if (v != Verdict::kOk) {
            SKSE::log::info("[DUALRING] refused '{}': {}", NameOf(a_ring), VerdictText(v));
            return false;
        }
        auto* p  = RE::PlayerCharacter::GetSingleton();
        auto* c  = Carrier();
        auto* em = RE::ActorEquipManager::GetSingleton();
        if (!p || !c || !em) return false;

        // ★★Where it actually goes. The second slot cannot hold a ring on its
        // own -- the doll would show a gap with nothing above it -- so an empty
        // first slot is filled instead of refused, and that is also what makes
        // the drag symmetric.
        if (!FirstRing(p)) {
            if (g_ringId) {
                // ★SWAP. The player dragged the first ring onto the second
                // slot; the two trade places. The one on the carrier goes back
                // to the engine's own ring slot, and the incoming one takes the
                // carrier.
                auto* prev = RingById(g_ringId);
                TakeOffImpl(/*a_standalone=*/false);   // B4-4: handoff, no redraw
                if (prev) {
                    em->EquipObject(p, prev, nullptr, 1, nullptr,
                                    false, false, false, true);
                    SKSE::log::info("[DUALRING] swap: '{}' moved to the first slot",
                        NameOf(prev));
                }
            } else {
                // Nothing on either slot: this belongs on the FIRST one.
                em->EquipObject(p, a_ring, nullptr, 1, nullptr,
                                false, false, false, true);
                SKSE::log::info("[DUALRING] '{}' -> first slot (the second cannot be "
                                "filled alone)", NameOf(a_ring));
                return true;
            }
        }

        if (g_ringId) TakeOffImpl(/*a_standalone=*/false);   // B4-4: handoff

        const int slot = FreeSlot(p);
        if (slot < 0) return false;
        const std::uint32_t mask = 1u << slot;

        // ★★THE UNIT'S enchantment, taken from the list the drop resolved --
        // the form's when there is one, and this unit's own when there is not.
        // a_xl is no longer decoration: a caller that passes nullptr for a
        // per-unit enchantment gets RingEnch's form-wide guess instead of the
        // ring it actually clicked.
        auto* ench = a_ring->formEnchanting ? a_ring->formEnchanting
                                            : EnchOn(a_xl);
        const std::uint16_t sig = Grid::InstanceSigOf(a_xl);   // nullptr -> 0 (plain)
        // ★The uid comes off the list too. Recording the signature alone left
        // every later lookup asking a question ExtraForPool cannot answer for a
        // uid-bearing unit -- see SecondUid.
        std::uint16_t uid = 0;
        if (a_xl) {
            if (const auto* xu = a_xl->GetByType<RE::ExtraUniqueID>()) uid = xu->uniqueID;
        }
        if (!ench) ench = RingEnch(a_ring, uid, sig);

        Lend(c, ench, mask);
        // The carrier is not the player's item; it goes in the pack purely so
        // the engine has something to equip, and comes back out on removal.
        p->AddObjectToContainer(c, nullptr, 1, nullptr);
        em->EquipObject(p, c, nullptr, 1, nullptr, false, false, false, true);

        g_ringId = a_ring->GetFormID();
        g_ringSig = sig;
        g_ringUid = uid;
        // B4-2b: the ring's own equip never runs on this path -- the carrier
        // stands in -- so the worn ledger's pending for it would go stale
        // (measured, round one of the state machine). Withdraw it here.
        WornLedger::CancelPending(g_ringId);
        // ★B4-4: and the GRID's equip-queue entry retires here too -- this
        // moment IS the carrier route's landing. Waiting for the TTL let a
        // swap spam pile up entries that each excluded one more unit of the
        // form, and a same-form spare in the pack blinked out until the sweep
        // (user report). The ring2 exclusion takes over seamlessly: the
        // none_of guard that was waiting on this entry opens the instant it
        // goes.
        Grid::ReleasePendingEquipFor(g_ringId);
        // ★The carrier bypasses the engine's equip of the RING itself, which
        // is where the vanilla equip sound lives -- so the second slot wore
        // rings in total silence (user report). The pickup clink is the same
        // substitute every board action already uses.
        p->PlayPickUpSound(a_ring, true, false);
        // ★The log says WHERE the enchantment came from. "none" on an item the
        // player can see an affix on is the signature of this whole class of
        // bug, and the form/unit split is the first thing to check.
        SKSE::log::info("[DUALRING] second ring '{}' on slot {} (0x{:08X}), "
                        "ench '{}' ({}, uid {:#06x} sig {:#06x})",
            NameOf(a_ring), slot + 30, mask, ench ? NameOf(ench) : "none",
            !ench                      ? "unenchanted"
          : a_ring->formEnchanting     ? "on the form"
                                       : "on the unit", uid, sig);
        return true;
    }

    void TakeOff() { TakeOffImpl(/*a_standalone=*/true); }

    void TakeOffImpl(bool a_standalone)
    {
        if (!g_ringId) return;
        auto* ring = RingById(g_ringId);
        auto* p    = RE::PlayerCharacter::GetSingleton();
        auto* c    = Carrier();
        if (p && c) {
            if (auto* em = RE::ActorEquipManager::GetSingleton()) {
                em->UnequipObject(p, c, nullptr, 1, nullptr, false, false, false, true);
            }
            // Count high enough to sweep duplicates a crossed save could leave.
            p->RemoveItem(c, 99, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        }
        Reclaim();
        // ★Both INSIDE the gate (규칙 6): every STANDALONE caller needs them.
        // The sound for the same reason as Wear's; the rebuild because the
        // carrier's stand-down is a board return with NO engine unequip event
        // -- the !rbdrop interrogation of Grid.cpp:11162 measured the ring
        // vanishing until the next unrelated rebuild without it.
        // ★★B4-4: the HANDOFF calls inside Wear are the exception the old
        // "every caller" claim missed. In a swap the displaced ring goes to
        // the CURSOR (WholeOnDoll starts that carry) or straight onto the
        // FIRST slot -- either way it never lands on the board here, so the
        // redraw painted a frame in the middle of the exclusion handoff for
        // nothing. That mid-swap frame is the deferred ring-blink's habitat
        // (survived the worn clocks and the counter absorption -- the
        // rebuild ITSELF was the remaining suspect).
        const bool wantDraw = a_standalone || !Grid::CarrierCarryActive();
        // ★The quiet handoff assumed the displaced ring rides the cursor.
        // True for the DROP swap -- its carry starts before Wear runs --
        // and false for the right-click ROUTER, which displaces with no
        // carry at all: the old ring went back to the pack with nothing
        // to redraw it, and right-clicking through several rings appeared
        // to wear them all (user report). No carrier carry up means the
        // return still needs its draw.
        if (a_standalone && p && ring) p->PlayPickUpSound(ring, false, false);
        const RE::FormID retId = g_ringId;
        SKSE::log::info("[DUALRING] second ring '{}' removed{}", NameOf(ring),
                        a_standalone ? "" : " (handoff)");
        // ★Ring session: state DOWN before the draw -- the partial add asks
        // the ring2 exclusion, and with g_ringId still set it would hide the
        // very unit it is trying to draw ("nothing fresh").
        g_ringId = 0;
        g_ringSig = 0;
        g_ringUid = 0;
        if (wantDraw) {
            // One form's return, not a repaint: the full rebuild here ran in
            // the middle of the swap window (the blink's habitat). The
            // partial declines -> the old rebuild, same fallback bargain as
            // every B3 path.
            if (!Grid::OnFormDelta(retId)) Grid::RequestRebuild();
        }
    }

    void RequestTakeOff() { g_wantOff = true; }

    void CancelTakeOff() { g_wantOff = false; }

    bool TakeOffPending() { return g_wantOff; }

    void Tick()
    {
        if (g_wantOff) {
            g_wantOff = false;
            TakeOff();   // rebuild + sound live inside the gate now
        }
        if (!g_ringId) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p || !p->Is3DLoaded()) return;

        // ★The ring can leave without going through us -- sold, dropped, taken
        // by a script. Observing beats remembering: the moment it is not in the
        // pack the carrier is standing in for nothing.
        auto* ring = RingById(g_ringId);
        if (!ring) { TakeOff(); return; }
        bool have = false;
        // ★★★AND THE LOAN CAN GO STALE UNDER US, WHICH IS WORSE THAN WRONG.
        //
        // Wear lends the carrier a pointer to the ring's enchantment. For a
        // per-unit affix that is a CREATED object, and a created object is
        // refcounted -- so anything that rewrites the unit can free the very
        // form the carrier is still wearing.
        //
        // The arcane enchanter does exactly that, by design. Diablo In Skyrim
        // detaches every affix while the table is open and puts it back on
        // close; if the player enchanted the item the two are MERGED into a new
        // created enchantment and both originals are released
        // (Enchanting.cpp, MergeOnto -> DestroyEnchantment). The carrier would
        // then be wearing a pointer to a destroyed form -- and the second ring
        // would grant the effect the player just traded away.
        //
        // So the loan is OBSERVED here, in the walk this function already does,
        // on the same principle the inventory check above is written on:
        // observing beats remembering.
        RE::EnchantmentItem* now    = nullptr;
        std::uint16_t        nowSig = g_ringSig;
        std::uint16_t        nowUid = g_ringUid;
        bool                 lost   = false;
        for (const auto& [obj, data] : p->GetInventory(
                 [&](RE::TESBoundObject& o) { return &o == ring; })) {
            have = data.first > 0;
            auto* entry = data.second.get();
            // The unit the carry recorded, still where it was: the ordinary
            // case, and the whole of it for a ring nobody has touched.
            now = EnchOn(Grid::ExtraForPool(entry, g_ringUid, g_ringSig));
            // ★Nothing under that signature any more. Either the unit was
            // rewritten (the merge above) or the table is open right now and
            // the affix is detached -- and the two are told apart by whether a
            // candidate exists at all, not by asking the other mod.
            if (!now && entry && entry->extraLists) {
                RE::ExtraDataList* only = nullptr;
                int                seen = 0;
                for (auto* xl : *entry->extraLists) {
                    if (!xl || xl->HasType<RE::ExtraWorn>() ||
                        xl->HasType<RE::ExtraWornLeft>()) continue;
                    if (EnchOn(xl)) { only = xl; ++seen; }
                }
                // ★ONE candidate is an identification; two is a guess, and a
                // guess here puts another ring's effect on the player. With the
                // old signature naming nothing and no way to tell the survivors
                // apart, the honest move is to stand the second slot down --
                // the player re-wears it and the carry re-identifies cleanly.
                if (seen == 1) {
                    now    = EnchOn(only);
                    nowSig = Grid::InstanceSigOf(only);
                    nowUid = 0;
                    if (const auto* xu = only->GetByType<RE::ExtraUniqueID>()) {
                        nowUid = xu->uniqueID;
                    }
                } else if (seen > 1) {
                    lost = true;
                }
            }
            (void)obj;
            break;
        }
        if (!have) {
            SKSE::log::info("[DUALRING] the second ring left the inventory -- dropping it");
            TakeOff();
            return;
        }
        if (lost) {
            SKSE::log::warn("[DUALRING] the second ring's unit can no longer be "
                            "identified (sig {:#06x} names nothing, and more than one "
                            "sibling carries an enchantment) -- standing it down",
                            g_ringSig);
            TakeOff();
            return;
        }
        // ★NULL IS NEVER RE-LENT OVER SOMETHING. A detached affix reads as null
        // for exactly as long as the enchanting menu is open, and reacting to
        // that would strip the carrier and put it back a moment later -- churn
        // the player would see, for a window that repairs itself.
        auto* c = Carrier();
        if (!c || !now || c->formEnchanting == now) return;

        // ★The SAME biped slot, deliberately. FreeSlot's pick is a modlist
        // negotiation (see its note); re-running it here could move the carrier
        // to a slot some other system watches, for a reason that has nothing to
        // do with slots.
        std::uint32_t mask = static_cast<std::uint32_t>(c->GetSlotMask().get());
        if (mask == 0) {
            const int slot = FreeSlot(p);
            if (slot < 0) return;
            mask = 1u << slot;
        }
        // ★★AND IT HAS TO BE RE-EQUIPPED, not merely re-lent. The engine reads
        // formEnchanting when the item goes ON; writing a new one underneath a
        // worn carrier changes nothing the actor is doing. Unequip, lend, equip
        // -- the carrier has no model and no sound, so the reseat is invisible.
        if (auto* em = RE::ActorEquipManager::GetSingleton()) {
            em->UnequipObject(p, c, nullptr, 1, nullptr, false, false, false, true);
            Lend(c, now, mask);
            em->EquipObject(p, c, nullptr, 1, nullptr, false, false, false, true);
        }
        const std::uint16_t wasUid = g_ringUid;
        const std::uint16_t wasSig = g_ringSig;
        g_ringSig = nowSig;
        g_ringUid = nowUid;
        SKSE::log::info("[DUALRING] the second ring's enchantment changed under the "
                        "carrier -- re-lent '{}' (uid {:#06x} sig {:#06x} -> "
                        "uid {:#06x} sig {:#06x})",
                        NameOf(now), wasUid, wasSig, nowUid, nowSig);
        // The signature is the board's off-board exclusion key and the doll's
        // display key; a rebuild is what makes both read the new one.
        Grid::RequestRebuild();
    }

    void RevertGame(SKSE::SerializationInterface*)
    {
        Reclaim();
        g_ringId  = 0;
        g_wantOff = false;
        // ★The signature travels with the id or not at all. The cosave stores
        // only g_ringId, so leaving this behind let a PREVIOUS session's
        // signature meet a freshly loaded id -- and since SecondSig() is the
        // board's off-board exclusion key, the ring actually being worn stayed
        // on the board while an innocent sibling unit vanished. Vanilla rings
        // hash to 0 and never showed it; player-enchanted ones always did.
        g_ringSig = 0;
        g_ringUid = 0;
    }

    void OnLoad()
    {
        if (!g_ringId) return;
        // ★The equip survived in the save; the LOAN did not. The engine re-read
        // the carrier from the plugin, so its enchantment is the record's own
        // again -- and a carrier worn with no enchantment is a second ring that
        // quietly stopped working.
        auto* p    = RE::PlayerCharacter::GetSingleton();
        auto* c    = Carrier();
        auto* ring = RingById(g_ringId);
        if (!p || !c || !ring) { g_ringId = 0; return; }
        g_lent = {};   // whatever we recorded belongs to the previous session
        // ★★★DO NOT TRUST THE MASK THAT CAME BACK.
        //
        // This read the carrier's CURRENT slot mask and wrote it straight back,
        // on the assumption that it was still the one Wear picked. It is not:
        // the same restore that reverted the enchantment reverted the mask too,
        // and the carrier's record is a costume anchor whose template carries a
        // circlet's wardrobe -- BOD2 on the HAIR and circlet slots. So the load
        // path took the authored hair mask and re-applied it, every time.
        //
        // It also never consulted FreeSlot, which is why "!ring2slot" appeared
        // to do nothing: the override is read by Wear, and a ring already worn
        // in the save never goes through Wear. Confirmed by a reporter's log --
        // the pin was applied at startup ("carrier slot pinned to 55") and the
        // character was bald anyway.
        //
        // Choose a slot the way Wear does, from what the body is actually
        // wearing right now, and let the override have its say.
        int slot = FreeSlot(p);
        if (slot < 0) {
            // Nothing free is not a reason to fall back on the record's own
            // mask -- that is the hair. Take the top of the scan and say so.
            slot = static_cast<int>(kSlots) - 3;
            SKSE::log::warn("[DUALRING] load: no free biped slot -- parking on {} anyway",
                            slot + 30);
        }
        // ★The UNIT's enchantment again, not the form's -- the same resolve
        // Wear does, for the same reason. A per-unit affix re-lent as
        // `ring->formEnchanting` is a null, and this path is the one that puts
        // the loan back after EVERY load: getting it wrong here would have
        // undone the fix on the next save the player made.
        auto* ench = RingEnch(ring, g_ringUid, g_ringSig);
        Lend(c, ench, 1u << slot);
        SKSE::log::info("[DUALRING] load: re-lent '{}' to the carrier on slot {} "
                        "(0x{:08X}), ench '{}' (uid {:#06x} sig {:#06x})",
            NameOf(ring), slot + 30, 1u << slot, ench ? NameOf(ench) : "none",
            g_ringUid, g_ringSig);
    }

    // ★★★VERSION 3 SAVES THE WHOLE UNIT NAME, and it is not bookkeeping.
    //
    // The record held only the ring's FormID, so the name of the UNIT on the
    // carrier -- uid and signature -- was per-session and reset to 0 on every
    // load (see RevertGame, which had to say so out loud to stop a previous
    // session's signature meeting a fresh id).
    //
    // Everything that asks the carrier "which unit?" therefore got nothing back
    // after a load: the board's off-board exclusion, the doll's glow and
    // tooltip, and the re-lend. For a plain ring that is the true answer and
    // nothing showed. For a per-unit enchantment it never is -- so the one kind
    // of ring this whole fix is about was also the one kind the load path could
    // not identify.
    //
    // ★Version 1 and 2 records still load. A v1 leaves both halves 0 and a v2
    // leaves the uid 0, which is exactly the state each of them saved;
    // RingEnch's form-wide fallback covers both, and the first re-lend through
    // Tick adopts a proper name.
    void SaveGame(SKSE::SerializationInterface* a_intfc)
    {
        if (!a_intfc->OpenRecord(kRecordType, 3)) return;
        a_intfc->WriteRecordData(&g_ringId, sizeof(g_ringId));
        a_intfc->WriteRecordData(&g_ringSig, sizeof(g_ringSig));
        a_intfc->WriteRecordData(&g_ringUid, sizeof(g_ringUid));
    }

    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        RE::FormID id = 0;
        a_intfc->ReadRecordData(&id, sizeof(id));
        std::uint16_t sig = 0;
        std::uint16_t uid = 0;
        if (a_version >= 2) a_intfc->ReadRecordData(&sig, sizeof(sig));
        if (a_version >= 3) a_intfc->ReadRecordData(&uid, sizeof(uid));
        // ★Through the resolver: a load order change moves every FormID, and a
        // raw one would name a different item entirely.
        RE::FormID resolved = 0;
        g_ringId = (id && a_intfc->ResolveFormID(id, resolved)) ? resolved : 0;
        // ★The name travels with the id or not at all -- the same rule
        // RevertGame enforces from the other end.
        g_ringSig = g_ringId ? sig : 0;
        g_ringUid = g_ringId ? uid : 0;
    }
}
