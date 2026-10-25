/* Network player count ([Players] MaxPlayers).
 *
 * STATUS: research only. MaxPlayers stays 8 (the original); larger values are refused with a log line.
 *
 * The race engine itself is built for 11 riders: single player offers 0..10 opponents (FUN_0044f2c0 fills
 * OpponentsListBox, bound 0x44f41c 83 fb 0a; app+0x2d94 = AI count), and the season table (app+0x570, 11 x 0x50,
 * loops 0x45d7d5 / 0x45f538 bd/bf 0b), the replay recorder (0xf8c-byte object, 11 rider records of 0x124 at
 * +0x300), the radar (11 pointers at +0x134, ctor 0x520cff b9 0b), the standings overlays (FUN_0051f070 /
 * FUN_00520090, 11 pairs) and the rider-creation stack arrays (FUN_0041a770, FUN_004189c0) all hold 11.
 * The start grid is procedural (FUN_00422030, switch 0x4220e2): one start point and direction per track
 * (DefaultStartPosition, the gate, or the track file's start line) and riders spread by N, so any N fits.
 * Race types: 0 Stunt Quarry, 1 Enduro, 2 Nationals, 3 Supercross, 4 Tag, 5 Baja. Rider ids on the wire are
 * (dpid, sub-index byte); no 8-bit rider masks were found in the send sites.
 *
 * The 8 is the multiplayer plumbing only (below). Up to 11 riders with at most 8 humans (AI filling the rest)
 * needs FitAiCount raised to 11, the rider set-up table and the grid / network slots moved out of the static
 * app object (~120-150 [app+disp] / [this+disp] sites, each disp rewritten to a new buffer), message 0x83
 * extended for riders 8..10, and FUN_0041a770's int[8] at [esp+0xdc] (zeroed at 0x41b24f) redirected. More than
 * 8 humans adds the lobby statics (7 remote players), the AI groups (8 x 0xc), dwMaxPlayers, the humans < 8 check
 * and new NamePlayer / ButRdyPlayer widgets in UI\MPBASE.DAT. Past 11 every 11-sized structure above (and the
 * replay format) would have to grow too. None of it can be tried on a machine without DirectPlay, and a missed
 * site corrupts memory as soon as a 9th rider is set up.
 *
 * DirectPlay (Net.cpp, CNet at app+8):
 *   CoCreateInstance(CLSID_DirectPlay d1eb6d20-..., IID_IDirectPlay4A 0ab1c531-...) 0x4afee5 / 0x4b256b.
 *   CNet::Host 0x4b0ab0: DPSESSIONDESC2 on the stack, dwMaxPlayers = 8 at 0x4b0b2e
 *     (c7 44 24 38 08 00 00 00), guidApplication 184dbbc1-ff39-11cf-b112-00a0c90aeab6 (0x55c040), Open 0x4b0b3a.
 *   CNet::Join 0x4b0b70 (Open with the enumerated session), GetSessionDesc / SetSessionDesc 0x4b0c26..0x4b0c7d.
 *   Sends: SendEx (vtbl +0xc4) in the reliable-message wrapper 0x4b19a0..0x4b1a8e (flags 0x600, a one-byte
 *   header with flag bits 0x20/0x80); receive loop 0x4b4018 (Receive, vtbl +0x64), dispatch 0x4b4160.
 *   Player indices on the wire are bytes, so the packets themselves would not cap at 8; the receivers'
 *   tables do.
 *
 * Race size (all humans + AI):
 *   0x4f7e80 FitAiCount(humans, ai): returns 0 if humans >= 8 (0x4f7e8a, 83 fd 08), caps humans + ai at 8
 *     (0x4f7ecb 83 f9 08 / 0x4f7ed0 b8 08 00 00 00) over 8 AI "class" groups (0x4f7eb6 ba 08 00 00 00).
 *   app+0x18 / byte app+0x2da5 humans, app+0x2d98 AI, app+0x2158 = riders in the race.
 *
 * Tables in the application object (static, 0x68b2e8; pointer at 0x5743ac):
 *   game set-up sub-object at app+0x578 (ctor 0x527690):
 *     riders     app+0x215c, 8 x 0xf8 (sub+0x1be4; ctor loop 0x5276db bf 08 00 00 00): +0xd4 id, +0xd8 class,
 *                +0xdc name[16] ... (written 0x4f6fd7.., 0x4f712a..; read by the race set-up 0x41b4cd.. and the
 *                results / HUD code 0x420ff3.., 0x460461..)
 *     net slots  app+0x2ebc, 8 x 8 (sub+0x2944; loop 0x5276f6 b9 08 00 00 00): dpid, ready byte at +5
 *   Code addresses them as [app + disp] with the app reloaded from 0x5743ac each time, and as
 *   [this + disp] inside the set-up class; a scan of the disassembly lists the first kind (27 sites found by a
 *   simple register tracker; pointer-walking loops and the class methods come on top).
 *
 * Lobby (SelectGamePicProcs.cpp, statics):
 *   0x68fe48 remote players, 7 x 0x18 {dpid, name}, bounds cmp esi,0x68fef0 (0x4fa1ad) / 0x68fef4 (0x4f8745)
 *     / 0x68fef8; 0x68fefc and 0x68ff18 two 7-dword arrays (ready, colour); 0x68ff34 local state, 0x68ff38 the
 *     remote count, 0x68ff3c lobby state. AI groups: 8 x 0xc in the dialog at +0x7f68 (loop 0x4f7110).
 *   The rows are UI widgets from the screen layout: NamePlayer%d, ButRdyPlayer1..8 (0x579e04..0x579ec4).
 *   More rows need new widgets in the layout or rows squeezed into the 8 (DethKarz-style).
 *
 * Message 0x83 "start game" (FUN_004f71e0 sends 0x8d8 bytes from the stack, 0x4f79f5; FUN_004f8960 receives):
 *   the settings block with the slots at +8, AI groups +0x194, names [8][16] +0x218, string tables [8][0x40]
 *   +0x298 / +0x498 / +0x698, dword tables [8] +0x1f8 / +0x898 / +0x8b8. Message 0x0f (lobby settings, 0xb4
 *   bytes) packs AI and humans in 4 bits each (byte 6).
 *
 * Not limits after all: 0x43da90 is the input binding table (50 actions x keyboard / 8 joysticks / mouse), and
 *   the 8-entry loop at recorder+0x25c (0x419e18, 0x421767) is not the rider records (those are 11 at +0x300).
 *   StartGate.slb is only scenery; the grid never reads slots from it.
 *
 * Every machine in a network game would need the same MaxPlayers. */
#include "mcm2patch.h"

#define ORIGINAL_MAX 8

static const struct {
    uint32_t addr;
    uint8_t len;
    uint8_t bytes[8];
    const char *what;
} ANCHORS[] = {
    { 0x4b0b2e, 8, { 0xc7, 0x44, 0x24, 0x38, 0x08, 0x00, 0x00, 0x00 }, "DPSESSIONDESC2.dwMaxPlayers = 8 (CNet::Host)" },
    { 0x4f7e8a, 3, { 0x83, 0xfd, 0x08 }, "humans < 8 (FitAiCount)" },
    { 0x4f7ecb, 3, { 0x83, 0xf9, 0x08 }, "humans + AI < 8 (FitAiCount)" },
    { 0x4f7ed0, 5, { 0xb8, 0x08, 0x00, 0x00, 0x00 }, "AI = 8 - humans (FitAiCount)" },
    { 0x5276db, 5, { 0xbf, 0x08, 0x00, 0x00, 0x00 }, "8 rider records (game set-up ctor)" },
    { 0x5276f6, 5, { 0xb9, 0x08, 0x00, 0x00, 0x00 }, "8 network slots (game set-up ctor)" },
    { 0x4fa1ad, 6, { 0x81, 0xfe, 0xf0, 0xfe, 0x68, 0x00 }, "7 remote players (lobby list end)" },
    { 0x41b24f, 5, { 0xb9, 0x08, 0x00, 0x00, 0x00 }, "8 per-rider flags (race set-up stack array)" },
    { 0x44f41c, 3, { 0x83, 0xfb, 0x0a }, "0..10 opponents (single player list)" },
};

void mcm_players_install(void)
{
    int want = mcm_ini_int("Players", "MaxPlayers", ORIGINAL_MAX);
    size_t i, missing = 0;
    for (i = 0; i < sizeof ANCHORS / sizeof ANCHORS[0]; i++) {
        if (!mcm_expect(ANCHORS[i].addr, ANCHORS[i].bytes, ANCHORS[i].len)) {
            mcm_log("players: %08x (%s) is not the original code", ANCHORS[i].addr, ANCHORS[i].what);
            missing++;
        }
    }
    if (want != ORIGINAL_MAX) {
        mcm_log("players: MaxPlayers=%d is not supported yet; races keep the game's 8 riders (human + AI). "
                "The 8 is part of the race tables, the lobby, the replay format and the start gate, not just the "
                "DirectPlay session (see players.c)", want);
    }
    if (missing) {
        mcm_log("players: %u of the known player-limit sites differ from the supported MCM2.EXE",
                (unsigned)missing);
    }
}
