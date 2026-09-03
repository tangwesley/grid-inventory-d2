#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 1.4 / B2 -- the request ledger. This is permanent wiring since the A-section
// repairs, and is on by default: the two-phase slot drop (B3-b) completes only
// through Confirm/Expire, so a ledger that defaulted to off left the commit
// half with no owner. "!ledger = 0" is a per-session escape hatch for bisecting
// a report, and never a shipping configuration.
//
// Every request we make of the engine is written down here BEFORE the call, and
// struck off when the engine's event confirms it. This is condition (A) of PLAN
// section 7, and B0 turned it from a nice idea into a requirement:
//
//   A confirmation is ONE REQUEST CONSUMING ONE EVENT, never a count of
//   events. Using a poison or a potion produces TWO ContainerChanged events for
//   one actual change (section 8-3, measured across five rounds). Anything that
//   counts arrivals concludes that consumables were confirmed twice -- and in
//   B3 that becomes a board losing two of everything it drinks.
//
// What B2 does NOT do yet is roll anything back. The board is still engine
// authority, so there is no speculative state to undo; OnExpire is where B3
// will hook that in. What B2 answers is how often a request is never confirmed
// at all, which is the number that says what section 5-2 actually costs.
//
// Ages are counted in FRAMES, not wall clock: the grid menu pauses the game,
// and a real-time deadline would expire requests while the player reads a
// tooltip. (CLAUDE.md's timer rule, same reasoning.)
namespace FUI::Ledger
{
    [[nodiscard]] bool Enabled();
    void               SetEnabled(bool a_on);

    // Called immediately before the engine call. a_delta is signed from the
    // PLAYER's point of view: +N arriving, -N leaving. uid/sig name the unit
    // when we know it -- B0 proved the engine's events never will (§8-2), so
    // this is the only place that knowledge exists.
    //
    // a_slot is the CELL this request empties, where the caller knows one. It
    // is the answer to section 8-5's "an undo needs the slot key on the
    // request", and it is what CommitSlotDrop and CancelSlotDrop act on -- so a
    // confirmation can only ever consume its OWN cell. A slotless request (a
    // world drop, a use) confirms without touching anyone's queue, which is
    // what stops a world drop from eating a pending store's key.
    void Submit(std::uint32_t a_form, std::int32_t a_delta, const char* a_who,
                std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                const std::string& a_slot = {});

    // An event arrived. Strikes off the OLDEST matching request and returns its
    // label, or nullptr when nothing matched (a genuine outside delta, or the
    // surplus half of a consumable pair).
    [[nodiscard]] const char* Confirm(std::uint32_t a_form, std::int32_t a_delta);

    // What a request looked like when it ran out of patience.
    struct Expired
    {
        std::uint32_t form;
        std::int32_t  delta;   // player's point of view, as submitted
        const char*   who;
        std::uint16_t uid;
        std::uint16_t sig;
        // Appended LAST -- the OffBoardUnit lesson. This is the cell the
        // request was emptying, and is empty when the caller had none.
        std::string   slot;
    };

    // B3-a: called when a request is never confirmed. This is the hook that
    // section 5-2 asks for, and it is deliberately the smallest thing that
    // closes the loop -- today the board is still engine authority, so recovery
    // means "ask for a rebuild" rather than "undo a speculative change".
    //
    // What it cannot do yet, and why B3-b exists: the record names a FORM, and
    // B2's first expiry logged uid 0000 sig 0000 (section 8-5). A form cannot
    // say WHICH cell to restore. The answer 1.3.0 had already reached is that
    // the key names the cell -- so an undo needs the slot key on the request,
    // not just the item's numbers.
    using ExpireFn = void (*)(const Expired&);
    void SetOnExpire(ExpireFn a_fn);

    // A request that DID land. Delivered from Tick and never from Confirm,
    // because events arrive on arbitrary threads (B0 saw five of them) and the
    // handler touches the board. Queueing them here also keeps them in arrival
    // order.
    using ConfirmFn = void (*)(const Expired&);
    void SetOnConfirm(ConfirmFn a_fn);


    // B4-3b: withdraw open OUTGOING entries of a form. This is the rollback
    // case, for requests struck from the queue before their engine call ever
    // ran -- a lost pickpocket roll force-closes the menu and clears everything
    // queued behind it. Oldest first, up to a_count units, with "use" excluded
    // as it is everywhere.
    //
    // Returns what was withdrawn so the CALLER can release each entry's slot
    // key through Grid::CancelSlotDrop. A cancelled request whose key stayed
    // queued would be consumed by that form's next confirmation, which is
    // exactly the bug the two-phase drop exists to prevent.
    [[nodiscard]] std::vector<Expired> Cancel(std::uint32_t a_form, int a_count,
                                              const char* a_why);

    // B4-3c: the open outgoing entries of ONE form, in the form the reconcile
    // reads them -- this replaced the removal counters. uid and sig are what
    // the request knew (rule 2). The list position deliberately does not travel
    // with them, per PLAN section 7: identity carries over, coordinates are
    // dropped.
    [[nodiscard]] std::vector<Expired> OpenOutgoingOf(std::uint32_t a_form);
    [[nodiscard]] int                  OpenOutgoingCount(std::uint32_t a_form);

    // One frame passed. Ages the outstanding requests and expires the stale.
    void Tick();

    // Report whatever is still outstanding and EXPIRE it -- through the same
    // OnExpire hook as a timeout, so the cell it was holding comes back.
    // (The first version cleared silently, which stranded the slot queue: the
    // request's key stayed queued forever and the NEXT confirmation of the
    // form consumed it -- a live tile lost its cell to a dead request.)
    // Landed-but-undelivered confirmations go out through OnConfirm first.
    void Flush(const char* a_why);

    // A load replaces the inventory without a single event -- requests from
    // before it can never be confirmed. Same lesson B0 §8-3 records.
    void Reset(const char* a_why);

    // ---- TEST ONLY: "!simrefuse = N" in GridInventory_ui.ini ----------------
    //
    // Arms N refusals. Each call returns true once and decrements, and the
    // caller then SKIPS its engine call while the request stays on the books --
    // so no confirmation can ever arrive and the expiry path runs for real.
    // Refuses to arm while the ledger is disabled ("!ledger = 0"), because a
    // skipped engine call with no ledger entry has no recovery path -- and that
    // is a state the game itself cannot produce (section 10-7).
    //
    // Why this has to exist at all: B2 measured ZERO refusals over two
    // sessions, because our own pre-checks (WouldOverflow, the follower cap,
    // the unnameable-unit refusal) stop nearly everything before the engine
    // ever sees it. A path that rare is exactly the one that will be untested
    // when it finally does fire. It models a real situation -- an engine call
    // that does not take -- which is what separates it from the kind of stress
    // tool section 10-7 warns against.
    //
    // Not persisted: it is a countdown, not a setting.
    [[nodiscard]] bool SimRefuse();
    void               SetSimRefuse(int a_count);
}
