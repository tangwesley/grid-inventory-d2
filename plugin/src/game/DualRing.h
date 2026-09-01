#pragma once

#include <cstdint>

// THE SECOND RING -- WORN FOR REAL.
//
// The engine only ever wears one ring. BipedAnim is `BIPOBJECT
// objects[kTotal]`, one item per slot, and kRing is a single bit -- so when two
// rings both claim that bit, the engine takes the first one off. Moving the
// slot does not help. We probed three ways (moving both masks, moving the ARMO
// mask only, and claiming no slot at all) and the engine removed the first ring
// every time.
//
// So instead of working around the contest, we end it: the ring already on the
// body gives up its kRing bit, and the incoming ring takes the slot with
// nothing left to displace. Both rings are then worn by the engine in every
// sense the rest of the plugin cares about -- ExtraWorn, active effects,
// tooltips, the doll walk, the board's worn accounting -- so none of those
// needs a special case for the second ring.
//
// WHY THIS REPLACED THE OLD "CARRIER" APPROACH.
// Version 1.5.x equipped an invisible costume anchor and copied the ring's
// `formEnchanting` onto it. That cannot work for a ring the player enchanted
// themselves, because a player enchantment lives on the unit's ExtraEnchantment
// and not on the record -- there was nothing to copy. The result was a ring
// with no effects at all, showing its unenchanted value (reported against
// 1.5.0: "Enchantment disappear after equipping", value dropping 4362 -> 500).
// Wearing the real item cannot reproduce that bug. The technique we use is
// Jewelry Limiter's (dylbill, Nexus 22098), which does the same thing from
// Papyrus with RemoveSlotFromMask.
//
// THE SECOND RING IS STILL NOT DRAWN ON THE HAND.
// An ARMO with no biped slot gives BipedAnim nowhere to put it, so the ring
// that gave up its bit is invisible. A ring's mesh also has slot 36 baked into
// the NIF, and the first ring holds that slot. We tried two routes -- going
// through the equipment system, and hand-loading the model through BSModelDB,
// which comes back `skin=NO`. A third route does exist: Left Hand Rings SKSE
// deep-copies the part the engine has already skinned, rewrites
// `NiSkinInstance::bones` onto another finger, and forces the partition on with
// `BSDismemberSkinInstance::UpdateDismemberPartion(slot, true)`. That is a
// sizeable piece of work and we have not attempted it -- but it is possible,
// and this comment should not claim otherwise.
//
// NOTHING IS SAVED ACROSS A LOAD, on purpose. The engine re-reads ARMO records
// from the plugin on every load (measured -- it is also why the old carrier had
// to re-copy its enchantment there), so the slot bits we removed are already
// restored by the time a save opens. Rather than serialise a debt the load has
// already cancelled, we re-derive the arrangement from the body itself. See
// Tick.
namespace FUI::DualRing
{
    // ---- there are no refusals --------------------------------------------
    // A ring dropped on either cell goes on. Nothing here rejects one.
    //
    // Two rules used to live here -- "not another unit of a form you are
    // already wearing" and "not the same magic effect twice" -- and both were
    // removed for the same reason: each only ran on the second cell's drop,
    // while the right-hand cell, an ordinary click and the quick wheel all let
    // the same pair through. A rule enforced on only some of the paths is worse
    // than no rule at all: it inconveniences the player who happens to hit it
    // and does nothing about the player who does not.
    //
    // The duplicate-effect rule also had a technical reason once, and lost it.
    // Under the carrier, a single stand-in wore a borrowed enchantment, so two
    // rings with one effect really was a broken state. Both rings are worn by
    // the engine now and the game applies both effects correctly (measured).
    // Whether wearing two of the same effect *should* be allowed is a balance
    // question, and vanilla never had to answer it because vanilla wears one
    // ring.
    //
    // Removing that rule also removed a whole class of bugs. The effect test
    // was the only reason the drop gate had to work out which unit was arriving
    // -- and a plain unit has no uid and no list index to be identified by, so
    // the gate regularly picked the wrong one. It would refuse a plain ring
    // because of an enchanted sibling's enchantment, and only when the two were
    // put on in one particular order (measured).

    // ---- which ring is which ----------------------------------------------
    // Does this ring hold the engine's ring slot -- is it the one you can
    // actually see on the hand? This is what the doll's two ring cells show:
    //
    //     ringR = the ring holding kRing (drawn on the hand)
    //     ringL = the ring that gave the slot up
    //
    // These cells used to mean nothing physical. The doll put whichever worn
    // ring the inventory walk happened to reach first into ringR -- hash order
    // -- while the slot bit went to the lowest FormID. "The visible ring" and
    // "the right-hand cell" were two independent answers that merely tended to
    // agree. When they disagreed, a drop aimed at one ring and the engine
    // displaced the other (measured 2026-09-01: the cursor picked up a ring
    // that was still worn, and the board needed a full rebuild to recover).
    [[nodiscard]] bool HoldsRingSlot(const RE::TESObjectARMO* a_armo);

    // Which cell does this ring unit belong in? The cell is NOT the slot bit,
    // and tying the two together was wrong in the other direction.
    //
    // The bit moves for mechanical reasons -- an equip takes it off whatever
    // stays so the incoming ring can join -- so the newest ring always ends up
    // holding it. With "cell = bit holder", every new ring appeared on the
    // right and pushed the old one to the left, so a ring dropped on the LEFT
    // cell showed up on the RIGHT and the pair looked like it had swapped
    // places for no reason (user report).
    //
    // So a cell is remembered as PLACEMENT -- where the player put the ring --
    // and the slot bit is left free to do its own job underneath. The two
    // answer different questions and neither has to follow the other.
    //
    // Placement is keyed by form AND content signature, so a plain ring and an
    // enchanted ring of the same kind keep their own cells. Two units matching
    // on both are interchangeable to every reader here, and either may take the
    // seat.
    //
    // Writing the placement is private: PrepareForEquip records it as part of
    // the same action, which is the only moment anything knows where the player
    // put a ring. The setter was public back when callers still had to call
    // three functions in the right order by hand; leaving it public now would
    // invite exactly the fragile protocol that consolidation removed.
    //
    // Returns false both for "belongs in the right cell" and for "no opinion".
    // The doll then falls back to the slot bit, which is the right answer for a
    // pair the player never placed by hand.
    [[nodiscard]] bool IsSecondCell(const RE::TESObjectARMO* a_armo, std::uint16_t a_sig);

    // Has the player placed anything in the left cell at all? The doll needs
    // this as a separate question, because its walk runs in inventory order: an
    // unplaced ring can reach the cell first and take the seat that a
    // deliberately placed ring was promised.
    [[nodiscard]] bool HasSecondCell();

    // ---- the one action ---------------------------------------------------
    // A ring is about to be equipped. Leave the body ready for it.
    //
    //   a_incoming    the ring going on
    //   a_sig         its content signature, which names the unit
    //   a_aimed       the unit the player pointed at -- the cell's current
    //                 occupant, given as the worn list that names it. Null for
    //                 a click or the quick wheel, which point at nothing
    //   a_secondCell  the drop named the LEFT cell
    //
    // This takes off whatever has to leave, moves the slot bit around so the
    // engine has nothing to displace, and records the placement. The caller
    // must then SKIP its own conflict pass for rings: everything is already
    // arranged and that pass would undo it.
    //
    // a_aimed is a UNIT, and it used to be a form. That difference was a bug
    // the player could see. The cell's occupant arrived here as
    // `occ->As<ARMO>()`, so when two units of one form were on the body, "the
    // ring in this cell" matched whichever came first in inventory order. The
    // wrong ring came off, the board handed the cursor the ring it believed it
    // had displaced, and that ring was actually still on the finger -- leaving
    // the player with a phantom they could only drop into nothing, while the
    // other ring went quietly into the pack (user report).
    //
    // Two units alike in both uid and signature stay interchangeable: removing
    // either is the same act to every reader here, the player included. So
    // naming the unit by pool is exact wherever exactness is possible, and
    // arbitrary only where there is no difference to get wrong.
    //
    // This used to be three functions and a protocol. MakeRoom, TakeOneOff and
    // NoteSecondCell had to be called in the right order by every path a ring
    // could arrive on, and each caller had to know which path it was. The order
    // was got wrong four times in a single day: a click path that skipped the
    // cap, a drop path that skipped it too, a missing second MakeRoom, and a
    // swap that removed a ring the caller was already removing. A caller that
    // has to know a sequence is a caller that will eventually get it wrong.
    //
    // The engine cannot help with any of this. Its conflict pass removes every
    // worn ring whose slot mask overlaps the incoming ring's, and the slot bit
    // is a fact about the FORM rather than about a unit -- so the pass can be
    // aimed at "all" or at "none", but never at "that one". Three attempts to
    // steer it produced three different failures: a phantom ring on the cursor,
    // both rings coming off, and a third ring going on (all measured
    // 2026-09-01). We do the removal ourselves and keep that pass away from
    // rings entirely.
    void PrepareForEquip(RE::TESObjectARMO* a_incoming, std::uint16_t a_sig,
                         RE::ExtraDataList* a_aimed, bool a_secondCell);

    // ---- taking one off ---------------------------------------------------
    // Take one ring off without disturbing the other rings' magic.
    //
    // THE ENGINE DISPELS BY ENCHANTMENT, NOT BY UNIT. An ActiveEffect records
    // `spell` (the EnchantmentItem) and `source` (the item form), and neither
    // of those identifies a unit. Two identical enchanted rings therefore raise
    // two effects that cannot be told apart, and MagicTarget::DispelEffect
    // takes a spell and a caster. Unequipping one of them dispels BOTH: the
    // ring still on the finger keeps its tooltip, its value and its place on
    // the doll, and silently stops doing anything (user report). Vanilla never
    // had to solve this, because vanilla wears one ring.
    //
    // There is no counterpart to Actor::DispelWornItemEnchantments to undo it
    // with -- the engine can clear worn enchantments but has no "apply them
    // again" -- so we simply put the surviving ring back on. Equipping is what
    // establishes the effect in the first place, and re-establishing it is all
    // this does.
    //
    // This is deliberately not the sweep's job, even though every other repair
    // in this file lives there. The sweep OBSERVES the body, and "this ring's
    // effect is missing" is not a fact about the body: an effect can be
    // legitimately absent because it was dispelled, resisted, or its conditions
    // are false. A sweep that read absence as damage would re-equip rings
    // nobody touched, forever. This particular collateral has exactly one known
    // cause, so it is repaired at that cause.
    //
    // Every path by which a ring LEAVES has to come through here, for the same
    // reason every path it arrives on goes through PrepareForEquip: a rule kept
    // on only some of the paths is worse than no rule.
    void RemoveWornUnit(RE::TESObjectARMO* a_armo, RE::ExtraDataList* a_xl);

    // ---- lifecycle --------------------------------------------------------
    // Runs once per game update. It OBSERVES rather than remembers, because the
    // world changes behind this system's back: a ring can be sold, dropped,
    // taken by a script, or a save can be loaded with two already on. There is
    // one invariant, restored from whatever the body is actually wearing:
    //
    //     IF ANY RING IS WORN, EXACTLY ONE OF THEM HOLDS kRing.
    //
    // That single rule covers every direction at once. A bit is handed back
    // when its ring leaves; a pair that a load put back into contest is
    // separated again; and a ring left slotless by the removal of the visible
    // ring gets the slot back instead of staying invisible for the rest of the
    // save.
    //
    // This is not a per-frame walk. It sleeps until something asks for it, and
    // then runs on a slow heartbeat only while a bit is out on loan.
    void Tick();

    // Hand every borrowed bit back, then ask for a fresh sweep.
    // Called on a new game and before a load.
    void RevertGame();
}
