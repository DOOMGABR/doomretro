/*
====================================================================

DOOM RETRO
A classic, refined DOOM source port. For Windows PC.

Copyright � 1993-1996 id Software LLC, a ZeniMax Media company.
Copyright � 2005-2014 Simon Howard.
Copyright � 2013-2014 Brad Harding.

This file is part of DOOM RETRO.

DOOM RETRO is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

DOOM RETRO is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with DOOM RETRO. If not, see http://www.gnu.org/licenses/.

====================================================================
*/

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <Commdlg.h>
#include "am_map.h"
#include "d_iwad.h"
#include "d_main.h"
#include "doomstat.h"
#include "f_finale.h"
#include "f_wipe.h"
#include "g_game.h"
#include "hu_stuff.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_menu.h"
#include "m_misc.h"
#include "p_saveg.h"
#include "p_setup.h"
#include "r_local.h"
#include "s_sound.h"
#include "SDL.h"
#include "st_stuff.h"
#include "v_video.h"
#include "w_merge.h"
#include "w_wad.h"
#include "wi_stuff.h"
#include "z_zone.h"

//
// D-DoomLoop()
// Not a globally visible function,
//  just included for source reference,
//  called by D_DoomMain, never exits.
// Manages timing and IO,
//  calls all ?_Responder, ?_Ticker, and ?_Drawer,
//  calls I_GetTime, I_StartFrame, and I_StartTic
//
void D_DoomLoop(void);

// Location where savegames are stored

char           *savegamedir;

// location of IWAD and WAD files

char           *iwadfile;

char           *wadfolder = ".";

boolean        devparm;        // started game with -devparm
boolean        nomonsters;     // checkparm of -nomonsters
boolean        respawnparm;    // checkparm of -respawn
boolean        fastparm;       // checkparm of -fast

skill_t        startskill;
int            startepisode;
int            startmap;
boolean        autostart;
int            startloadgame;

boolean        advancedemo;

void D_CheckNetGame(void);

//
// EVENT HANDLING
//
// Events are asynchronous inputs generally generated by the game user.
// Events can be discarded if no responder claims them
//
#define MAXEVENTS 64
static event_t    events[MAXEVENTS];
static int        eventhead;
static int        eventtail;

//
// D_PostEvent
// Called by the I/O functions when input is detected
//
void D_PostEvent(event_t *ev)
{
    events[eventhead] = *ev;
    eventhead = (eventhead + 1) % MAXEVENTS;
}

//
// D_PopEvent
// Read an event from the queue
//
event_t *D_PopEvent(void)
{
    event_t *result;

    if (eventtail == eventhead)
        return NULL;

    result = &events[eventtail];

    eventtail = (eventtail + 1) % MAXEVENTS;

    return result;
}

boolean wipe = true;

//
// D_ProcessEvents
// Send all the events of the given timestamp down the responder chain
//
void D_ProcessEvents(void)
{
    event_t *ev;

    while ((ev = D_PopEvent()) != NULL)
    {
        if (wipe && ev->type == ev_mouse)
            continue;
        if (M_Responder(ev))
            continue;           // menu ate the event
        G_Responder(ev);
    }
}

//
// D_Display
//  draw current display, possibly wiping it from the previous
//

// wipegamestate can be set to -1 to force a wipe on the next draw
gamestate_t     wipegamestate = GS_DEMOSCREEN;
extern  boolean setsizeneeded;

void R_ExecuteSetViewSize(void);

void D_Display(void)
{
    static boolean     viewactivestate = false;
    static boolean     menuactivestate = false;
    static boolean     pausedstate = false;
    static gamestate_t oldgamestate = (gamestate_t)(-1);
    static int         borderdrawcount;
    int                nowtime;
    int                tics;
    int                wipestart;
    boolean            done;

    // change the view size if needed
    if (setsizeneeded)
    {
        R_ExecuteSetViewSize();
        oldgamestate = (gamestate_t)(-1);         // force background redraw
        borderdrawcount = 3;
    }

    // save the current screen if about to wipe
    if (gamestate != wipegamestate)
    {
        wipe = true;
        wipe_StartScreen();
        menuactive = false;
    }
    else
        wipe = false;

    if (gamestate == GS_LEVEL && gametic)
        HU_Erase();

    // do buffered drawing
    switch (gamestate)
    {
        case GS_LEVEL:
            if (!gametic)
                break;
            ST_Drawer(viewheight == SCREENHEIGHT, true);
            break;

        case GS_INTERMISSION:
            WI_Drawer();
            break;

        case GS_FINALE:
            F_Drawer();
            break;

        case GS_DEMOSCREEN:
            D_PageDrawer();
            break;
    }

    // draw the view directly
    if (gamestate == GS_LEVEL && gametic)
    {
        R_RenderPlayerView(&players[displayplayer]);
        if (automapactive)
            AM_Drawer();
        HU_Drawer();
    }

    // clean up border stuff
    if (gamestate != oldgamestate && gamestate != GS_LEVEL)
        I_SetPalette((byte *)W_CacheLumpName("PLAYPAL", PU_CACHE));

    // see if the border needs to be initially drawn
    if (gamestate == GS_LEVEL && oldgamestate != GS_LEVEL)
    {
        viewactivestate = false;        // view was not active
        R_FillBackScreen();             // draw the pattern into the back screen
    }

    // see if the border needs to be updated to the screen
    if (gamestate == GS_LEVEL && !automapactive && scaledviewwidth != SCREENWIDTH)
    {
        if (menuactive || menuactivestate || !viewactivestate || paused || pausedstate)
            borderdrawcount = 3;
        if (borderdrawcount)
        {
            R_DrawViewBorder();         // erase old menu stuff
            borderdrawcount--;
        }
    }

    menuactivestate = menuactive;
    viewactivestate = viewactive;
    oldgamestate = wipegamestate = gamestate;

    // draw pause pic
    if (paused)
    {
        M_DarkBackground();
        M_DrawCenteredString(viewwindowy / 2 + (viewheight / 2 - 16) / 2, "Paused");
        pausedstate = true;
    }
    else
        pausedstate = false;

    // menus go directly to the screen
    M_Drawer();                 // menu is drawn even on top of everything

    // normal update
    if (!wipe)
    {
        I_FinishUpdate();       // page flip or blit buffer
        return;
    }

    // wipe update
    wipe_EndScreen();

    wipestart = I_GetTime() - 1;

    do
    {
        do
        {
            nowtime = I_GetTime();
            tics = nowtime - wipestart;
            I_Sleep(1);
        } while (tics <= 0);

        wipestart = nowtime;
        done = wipe_ScreenWipe(tics);
        M_Drawer();             // menu is drawn even on top of wipes
        I_FinishUpdate();       // page flip or blit buffer
    } while (!done);
}

//
//  D_DoomLoop
//
void D_DoomLoop(void)
{
    if (demorecording)
        G_BeginRecording();

    TryRunTics();

    I_InitGraphics();

    R_ExecuteSetViewSize();

    D_StartGameLoop();

    while (1)
    {
        TryRunTics(); // will run at least one tic

        S_UpdateSounds(players[consoleplayer].mo); // move positional sounds

        // Update display, next frame, with current state.
        if (screenvisible)
            D_Display();
    }
}

//
//  DEMO LOOP
//
static int  demosequence;
static int  pagetic;
static char *pagename;

//
// D_PageTicker
// Handles timing for warped projection
//
void D_PageTicker(void)
{
    if (--pagetic < 0)
        D_AdvanceDemo();
    if (!TITLEPIC && !menuactive)
        M_StartControlPanel();
}

//
// D_PageDrawer
//
void D_PageDrawer(void)
{
    patch_t *patch = (patch_t *)W_CacheLumpName(pagename, PU_CACHE);

    if (patch)
        V_DrawPatch(0, 0, 0, patch);
    else
        memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);
}

//
// D_AdvanceDemo
// Called after each demo or intro demosequence finishes
//
void D_AdvanceDemo(void)
{
    advancedemo = true;
}

//
// This cycles through the demo sequences.
//
void D_DoAdvanceDemo(void)
{
    players[consoleplayer].playerstate = PST_LIVE;      // not reborn
    advancedemo = false;
    usergame = false;                                   // no save / end game here
    paused = false;
    gameaction = ga_nothing;

    //if (gamemode == retail)
    //    demosequence = (demosequence + 1) % 7;
    //else
    //    demosequence = (demosequence + 1) % 6;
    demosequence = 0;

    switch (demosequence)
    {
        case 0:
            if (gamemode == commercial)
                pagetic = TICRATE * 11;
            else
                pagetic = 170;
            gamestate = GS_DEMOSCREEN;
            pagename = (TITLEPIC ? "TITLEPIC" : (DMENUPIC ? "DMENUPIC" : "INTERPIC"));
            if (gamemode == commercial)
                S_StartMusic(mus_dm2ttl);
            else
                S_StartMusic(mus_intro);
            break;
        case 1:
            G_DeferredPlayDemo("demo1");
            break;
        case 2:
            pagetic = 200;
            gamestate = GS_DEMOSCREEN;
            pagename = "CREDIT";
            break;
        case 3:
            G_DeferredPlayDemo("demo2");
            break;
        case 4:
            gamestate = GS_DEMOSCREEN;
            if (gamemode == commercial)
            {
                pagetic = TICRATE * 11;
                pagename = (TITLEPIC ? "TITLEPIC" : (DMENUPIC ? "DMENUPIC" : "INTERPIC"));
                S_StartMusic(mus_dm2ttl);
            }
            else
            {
                pagetic = 200;

                if (gamemode == retail)
                    pagename = "CREDIT";
                else
                    pagename = "HELP2";
            }
            break;
        case 5:
            G_DeferredPlayDemo("demo3");
            break;
        // THE DEFINITIVE DOOM Special Edition demo
        case 6:
            G_DeferredPlayDemo("demo4");
            break;
    }
}

//
// D_StartTitle
//
void D_StartTitle(void)
{
    gameaction = ga_nothing;
    demosequence = -1;
    SDL_WM_SetCaption(gamedescription, NULL);
    D_AdvanceDemo();
}

static boolean D_AddFile(char *filename)
{
    wad_file_t *handle;

    handle = W_AddFile(filename);

    return (handle != NULL);
}

char *uppercase(char *str)
{
    char *newstr;
    char *p;

    p = newstr = strdup(str);
    while (*p++ = toupper(*p));

    return newstr;
}

// Initialize the game version

static void InitGameVersion(void)
{
    // Determine automatically

    if (gamemode == shareware || gamemode == registered)
        // original
        gameversion = exe_doom_1_9;
    else if (gamemode == retail)
        gameversion = exe_ultimate;
    else if (gamemode == commercial)
    {
        if (gamemission == doom2)
            gameversion = exe_doom_1_9;
        else
            // Final Doom: tnt or plutonia
            gameversion = exe_final;
    }

    // The original exe does not support retail - 4th episode not supported
    if (gameversion < exe_ultimate && gamemode == retail)
        gamemode = registered;

    // EXEs prior to the Final Doom exes do not support Final Doom.
    if (gameversion < exe_final && gamemode == commercial)
        gamemission = doom2;
}

void D_ToggleHiddenFile(char *filename, boolean hide)
{
    DWORD attributes = GetFileAttributes(filename);

    if (attributes == INVALID_FILE_ATTRIBUTES)
        return;

    if (hide)
        SetFileAttributes(filename, attributes | FILE_ATTRIBUTE_HIDDEN);
    else
        SetFileAttributes(filename, attributes & ~FILE_ATTRIBUTE_HIDDEN);
}

boolean D_ChooseIWAD(void)
{
    OPENFILENAME ofn;
    char         szFile[4096];
    int          iwadfound = false;

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.lpstrFile[0] = '\0';
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "IWAD/PWAD Files (*.wad)\0*.WAD\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = wadfolder;
    ofn.Flags = OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT
                | OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = "Where\u2019s All the Data?\0";

    if (GetOpenFileName(&ofn))
    {
        // only one file was selected
        if (!ofn.lpstrFile[strlen(ofn.lpstrFile) + 1])
        {
            char *file = M_ExtractFilename(ofn.lpstrFile);

            wadfolder = strdup(M_ExtractFolder(ofn.lpstrFile));

            // if it's NERVE.WAD, try to open DOOM2.WAD with it
            if (!strcasecmp(M_ExtractFilename(ofn.lpstrFile), "NERVE.WAD"))
            {
                static char fullpath[MAX_PATH];

                sprintf(fullpath, "%s\\DOOM2.WAD", wadfolder);
                IdentifyIWADByName(fullpath);
                if (D_AddFile(fullpath))
                    iwadfound = true;
                W_MergeFile(ofn.lpstrFile);
                modifiedgame = true;
                nerve = true;
            }

            // otherwise make sure it's an IWAD
            else if (!strcasecmp(file, "DOOM.WAD")
                    || !strcasecmp(file, "DOOM1.WAD")
                    || !strcasecmp(file, "DOOM2.WAD")
                    || !strcasecmp(file, "PLUTONIA.WAD")
                    || !strcasecmp(file, "TNT.WAD")
                    || W_WadType(ofn.lpstrFile) == IWAD)
            {
                IdentifyIWADByName(ofn.lpstrFile);
                D_AddFile(ofn.lpstrFile);
                iwadfound = true;
            }
        }

        // more than one file was selected
        else
        {
            LPSTR iwad = ofn.lpstrFile;
            LPSTR pwad = ofn.lpstrFile;

            wadfolder = strdup(szFile);

            // find and add iwad first
            while (iwad[0])
            {
                static char fullpath[MAX_PATH];

                iwad += lstrlen(iwad) + 1;
                sprintf(fullpath, "%s\\%s", wadfolder, iwad);

                if (!strcasecmp(iwad, "DOOM.WAD")
                    || !strcasecmp(iwad, "DOOM1.WAD")
                    || !strcasecmp(iwad, "DOOM2.WAD")
                    || !strcasecmp(iwad, "PLUTONIA.WAD")
                    || !strcasecmp(iwad, "TNT.WAD")
                    || W_WadType(fullpath) == IWAD)
                {
                    if (!iwadfound)
                    {
                        IdentifyIWADByName(iwad);
                        if (D_AddFile(fullpath))
                            iwadfound = true;
                    }
                }
            }

            // merge any pwads
            while (pwad[0])
            {
                static char fullpath[MAX_PATH];

                pwad += lstrlen(pwad) + 1;
                sprintf(fullpath, "%s\\%s", wadfolder, pwad);

                if (strcasecmp(pwad, "DOOMRETRO.WAD") && W_WadType(fullpath) == PWAD)
                    if (W_MergeFile(fullpath))
                    {
                        modifiedgame = true;
                        if (!strcasecmp(pwad, "NERVE.WAD"))
                            nerve = true;
                    }
            }
        }

        if (!iwadfound)
            I_Error("Game mode indeterminate. No valid IWAD was specified.");
    }
    return iwadfound;
}

//
// D_DoomMainSetup
//
// CPhipps - the old contents of D_DoomMain, but moved out of the main
//  line of execution so its stack space can be freed
static void D_DoomMainSetup(void)
{
    int     p;
    char    file[256];
    char    demolumpname[9];
    int     temp;
    boolean choseniwad = false;

    SDL_Init(0);

    M_FindResponseFile();

    iwadfile = D_FindIWAD();

    modifiedgame = false;

    nomonsters = M_CheckParm("-nomonsters");
    respawnparm = M_CheckParm("-respawn");
    fastparm = M_CheckParm("-fast");
    devparm = M_CheckParm("-devparm");
    if (M_CheckParm("-altdeath"))
        deathmatch = 2;
    else if (M_CheckParm("-deathmatch"))
        deathmatch = 1;

    M_SetConfigDir();

    // turbo option
    p = M_CheckParm("-turbo");
    if (p)
    {
        int        scale = 200;
        extern int forwardmove[2];
        extern int sidemove[2];

        if (p < myargc - 1)
            scale = atoi(myargv[p + 1]);
        if (scale < 10)
            scale = 10;
        if (scale > 400)
            scale = 400;
        forwardmove[0] = forwardmove[0] * scale / 100;
        forwardmove[1] = forwardmove[1] * scale / 100;
        sidemove[0] = sidemove[0] * scale / 100;
        sidemove[1] = sidemove[1] * scale / 100;
    }

    // init subsystems
    V_Init();

    // Load configuration files before initialising other subsystems.
    M_LoadDefaults();

    if (!M_FileExists("DOOMRETRO.WAD"))
        I_Error("Can't find doomretro.wad.");

    if (iwadfile)
        D_AddFile(iwadfile);
    else 
    {
        D_ToggleHiddenFile("DOOMRETRO.WAD", true);

        choseniwad = D_ChooseIWAD();

        D_ToggleHiddenFile("DOOMRETRO.WAD", false);

        if (!choseniwad)
            exit(-1);

        M_SaveDefaults();
    }

    if (!W_MergeFile("DOOMRETRO.WAD"))
        I_Error("Can't find doomretro.wad.");

    if (W_CheckNumForName("BLD2A0") < 0)
        I_Error("Wrong version of doomretro.wad.");

    p = M_CheckParmWithArgs("-file", 1);
    if (p > 0)
    {
        for (p = p + 1; p < myargc && myargv[p][0] != '-'; ++p)
        {
            char *filename = uppercase(D_TryFindWADByName(myargv[p]));

            if (W_MergeFile(filename))
            {
                modifiedgame = true;
                if (!strcasecmp(filename, "NERVE.WAD"))
                    nerve = true;
            }
        }
    }

    M_NEWG = (W_CheckMultipleLumps("M_NEWG") > 1);
    M_EPISOD = (W_CheckMultipleLumps("M_EPISOD") > 1);
    M_SKILL = (W_CheckMultipleLumps("M_SKILL") > 1);
    M_SKULL1 = (W_CheckMultipleLumps("M_SKULL1") > 1);
    M_LGTTL = (W_CheckMultipleLumps("M_LGTTL") > 1);
    M_SGTTL = (W_CheckMultipleLumps("M_SGTTL") > 1);
    M_SVOL = (W_CheckMultipleLumps("M_SVOL") > 1);
    M_OPTTTL = (W_CheckMultipleLumps("M_OPTTTL") > 1);
    M_MSGON = (W_CheckMultipleLumps("M_MSGON") > 1);
    M_MSGOFF = (W_CheckMultipleLumps("M_MSGOFF") > 1);
    M_NMARE = (W_CheckMultipleLumps("M_NMARE") > 1);
    M_MSENS = (W_CheckMultipleLumps("M_MSENS") > 1);
    STBAR    = (W_CheckMultipleLumps("STBAR") > 1);
    STCFN034 = (W_CheckMultipleLumps("STCFN034") > 1);
    STCFN039 = (W_CheckMultipleLumps("STCFN039") > 1);
    WISCRT2  = (W_CheckMultipleLumps("WISCRT2") > 1);
    STYSNUM0 = (W_CheckMultipleLumps("STYSNUM0") > 1);
    MAPINFO  = (W_CheckNumForName("MAPINFO") >= 0);
    TITLEPIC = (W_CheckNumForName("TITLEPIC") >= 0);
    DMENUPIC = (W_CheckNumForName("DMENUPIC") >= 0);

    bfgedition = (DMENUPIC && W_CheckNumForName("M_ACPT") >= 0);

    p = M_CheckParmWithArgs("-playdemo", 1);

    if (!p)
        p = M_CheckParmWithArgs("-timedemo", 1);

    if (p)
    {
        if (!strcasecmp(myargv[p + 1] + strlen(myargv[p + 1]) - 4, ".lmp"))
            strcpy(file, myargv[p + 1]);
        else
            sprintf(file, "%s.lmp", myargv[p + 1]);

        if (D_AddFile(file))
        {
            strncpy(demolumpname, lumpinfo[numlumps - 1].name, 8);
            demolumpname[8] = '\0';
        }
    }

    // Generate the WAD hash table.  Speed things up a bit.

    W_GenerateHashTable();

    D_IdentifyVersion();
    InitGameVersion();
    D_SetGameDescription();
    D_SetSaveGameDir();

    // Check for -file in shareware
    if (modifiedgame)
    {
        // These are the lumps that will be checked in IWAD,
        // if any one is not present, execution will be aborted.
        char name[23][9] =
        {
            "e2m1", "e2m2", "e2m3", "e2m4", "e2m5", "e2m6", "e2m7", "e2m8", "e2m9",
            "e3m1", "e3m3", "e3m3", "e3m4", "e3m5", "e3m6", "e3m7", "e3m8", "e3m9",
            "dphoof", "bfgga0", "heada1", "cybra1", "spida1d1"
        };
        int i;

        if (gamemode == shareware)
            I_Error("You cannot %s with the shareware version.\n"
                    "Please purchase the full version.",
                    choseniwad ? "open PWADs" : "use -FILE");

        // Check for fake IWAD with right name,
        // but w/o all the lumps of the registered version.
        if (gamemode == registered)
            for (i = 0; i < 23; i++)
                if (W_CheckNumForName(name[i]) < 0)
                    I_Error("This is not the registered version.");
    }

    // get skill / episode / map from parms
    startskill = sk_medium;
    startepisode = 1;
    startmap = 1;
    autostart = false;

    p = M_CheckParmWithArgs("-skill", 1);
    if (p)
    {
        temp = myargv[p + 1][0] - '1';
        if (temp >= sk_baby && temp <= sk_nightmare)
        {
            startskill = (skill_t)temp;
            autostart = true;
        }
    }

    p = M_CheckParmWithArgs("-episode", 1);
    if (p)
    {
        temp = myargv[p + 1][0] - '0';
        if ((gamemode == shareware && temp == 1)
            || (temp >= 1
                && ((gamemode == registered && temp <= 3)
                    || (gamemode == retail && temp <= 4))))
        {
            startepisode = temp;
            startmap = 1;
            autostart = true;
        }
    }

    timelimit = 0;

    p = M_CheckParmWithArgs("-timer", 1);
    if (p)
    {
        timelimit = atoi(myargv[p + 1]);
    }

    p = M_CheckParm("-avg");
    if (p)
    {
        timelimit = 20;
    }

    p = M_CheckParmWithArgs("-warp", 1);
    if (p)
    {
        if (gamemode == commercial)
            startmap = atoi(myargv[p + 1]);
        else
        {
            startepisode = myargv[p + 1][0] - '0';

            if (p + 2 < myargc)
                startmap = myargv[p + 2][0] - '0';
            else
                startmap = 1;
        }
        autostart = true;
    }


    p = M_CheckParmWithArgs("-loadgame", 1);
    if (p)
        startloadgame = atoi(myargv[p + 1]);
    else
        startloadgame = -1;

    if (mouseSensitivity < MOUSESENSITIVITY_MIN || mouseSensitivity > MOUSESENSITIVITY_MAX)
        mouseSensitivity = MOUSESENSITIVITY_DEFAULT;
    gamepadSensitivity = 1.25f + (float)mouseSensitivity / MOUSESENSITIVITY_MAX;

    if (sfxVolume < SFXVOLUME_MIN || sfxVolume > SFXVOLUME_MAX)
        sfxVolume = SFXVOLUME_DEFAULT;

    if (musicVolume < MUSICVOLUME_MIN || musicVolume > MUSICVOLUME_MAX)
        musicVolume = MUSICVOLUME_DEFAULT;

    if (screenblocks < SCREENBLOCKS_MIN || screenblocks > SCREENBLOCKS_MAX)
        screenblocks = SCREENBLOCKS_DEFAULT;
    if (widescreen && !fullscreen)
        screenblocks = 11;
    if (fullscreen && screenblocks == 11)
    {
        widescreen = true;
        screenblocks = 10;
    }

    if (usegamma < USEGAMMA_MIN || usegamma > USEGAMMA_MAX)
        usegamma = USEGAMMA_DEFAULT;

    M_Init();

    R_Init();

    P_Init();

    I_Init();

    S_Init((int)(sfxVolume * (127.0f / 15.0f)), (int)(musicVolume * (127.0f / 15.0f)));

    D_CheckNetGame();

    HU_Init();

    ST_Init();

    AM_Init();

    p = M_CheckParmWithArgs("-record", 1);
    if (p)
    {
        G_RecordDemo(myargv[p + 1]);
        autostart = true;
    }

    p = M_CheckParmWithArgs("-playdemo", 1);
    if (p)
    {
        singledemo = true;                      // quit after one demo
        G_DeferredPlayDemo(demolumpname);
        D_DoomLoop();                           // never returns
    }

    p = M_CheckParmWithArgs("-timedemo", 1);
    if (p)
    {
        G_TimeDemo(demolumpname);
        D_DoomLoop();                           // never returns
    }

    if (startloadgame >= 0)
    {
        strcpy(file, P_SaveGameFile(startloadgame));
        G_LoadGame(file);
    }

    if (gameaction != ga_loadgame)
    {
        if (autostart || netgame)
            G_InitNew(startskill, startepisode, startmap);
        else
            D_StartTitle();                     // start up intro loop
    }
}

//
// D_DoomMain
//
void D_DoomMain(void)
{
    D_DoomMainSetup(); // CPhipps - setup out of main execution stack

    D_DoomLoop();                               // never returns
}
