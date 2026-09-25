/*
 * klattsch_gui.cpp -- the sample generator, first version: a Win32 window
 * over the C engine, for hearing it without a command line.
 *
 * klattsch is Tony Gies's work (MIT, see LICENSE); this program drives the C
 * translation of it. The text front end it also drives is BSD-3-Clause; see
 * NOTICE.md. The window's shape -- plain controls, a label immediately before
 * each one, the Tab fix for multi-line edits, playback off the UI thread, a
 * headless --selftest -- follows Votraxxion's gui-native, which is the
 * reference docs/GENERATOR.md names for what passes with a screen reader.
 *
 * What this version exposes is what the engine has today, by the decision of
 * 2026-09-25 (docs/ROADMAP.md, phase 4): the ten voice settings the compiler
 * takes as initial values, the phoneme bank and the sample rate. The
 * constants upstream froze stay frozen here until phase 3 returns.
 *
 * Plain Win32 controls throughout, deliberately. Every control is a
 * standard one, preceded in tab order by the static label that names it, so
 * a screen reader reads the label with the control; nothing is drawn by hand.
 */

#define WIN32_LEAN_AND_MEAN
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "kl_banks.h"
#include "kl_compile.h"
#include "kl_render.h"
#include "kl_text.h"
#include "kl_token.h"
#include "kl_version.h"
#include "kl_wav.h"
}

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#endif

/* --------------------------------------------------------------------- */
/* The voice settings                                                    */
/* --------------------------------------------------------------------- */

/*
 * One row per compiler option, in kl_opt order, so the label, the range, the
 * default and the conversion to the engine's unit live in one place.
 *
 * The controls are spin boxes, which hold integers, so fractional settings
 * are shown in a unit that makes them whole: scale, effort, aspiration, tilt
 * and tremolo depth are percentages. Every default converts exactly (50 / 100
 * is 0.5 as a double), so an untouched control gives the same number the
 * engine would have used on its own, and the same samples.
 *
 * Accelerators only on the two settings reached for most; the others are one
 * Tab away, and ten accelerators in one window collide.
 *
 * The last row is not a compiler option but a front-end one: the length of
 * a comma pause, which the front end writes as a `p` directive (kl_text.h).
 * Its `opt` is KL_OPT_COUNT, meaning "not passed to the compiler". It only
 * affects text mode; phoneme source says its own pauses.
 */
struct ParamSpec {
    const wchar_t *label;
    int lo, hi, def;
    double unit;        /* engine value = control value * unit */
    kl_opt opt;
};

static const ParamSpec PARAMS[] = {
    { L"Base &pitch (Hz):",         40,  600, 120, 1.0,  KL_OPT_BASE_F0 },
    { L"&Rate (ms per phoneme):",   20, 1000, 110, 1.0,  KL_OPT_RATE },
    { L"Formant scale (%):",        50,  200, 100, 0.01, KL_OPT_SCALE },
    { L"Vibrato depth (Hz):",        0,   50,   0, 1.0,  KL_OPT_VIBRATO_DEPTH },
    { L"Vibrato rate (Hz):",         1,   20,   5, 1.0,  KL_OPT_VIBRATO_RATE },
    { L"Tremolo depth (%):",         0,  100,   0, 0.01, KL_OPT_TREMOLO_DEPTH },
    { L"Tremolo rate (Hz):",         1,   20,   5, 1.0,  KL_OPT_TREMOLO_RATE },
    { L"Aspiration (%):",            0,  100,   0, 0.01, KL_OPT_ASPIRATION },
    { L"Spectral tilt (%):",       -95,   95,   0, 0.01, KL_OPT_TILT },
    { L"Effort (%):",                0,  100,  50, 0.01, KL_OPT_EFFORT },
    { L"Comma pause (ms):",         20, 1000, KL_TEXT_COMMA_MS, 1.0, KL_OPT_COUNT },
};
#define P_COMMA 10
#define PARAM_COUNT ((int)(sizeof PARAMS / sizeof PARAMS[0]))

static const int SAMPLE_RATES[] = { 8000, 11025, 16000, 22050, 44100, 48000 };
#define RATE_COUNT ((int)(sizeof SAMPLE_RATES / sizeof SAMPLE_RATES[0]))
#define DEFAULT_RATE_INDEX 5   /* 48000, what the CLI renders at */

struct Voice {
    int    bank;                 /* index into kl_banks */
    int    sampleRate;
    int    value[PARAM_COUNT];   /* control values, in control units */
};

static Voice DefaultVoice(void)
{
    Voice v;
    size_t i;
    v.bank = 0;
    for (i = 0; i < kl_bank_count; i++)
        if (strcmp(kl_banks[i].name, kl_default_bank) == 0)
            v.bank = (int)i;
    v.sampleRate = SAMPLE_RATES[DEFAULT_RATE_INDEX];
    for (int k = 0; k < PARAM_COUNT; k++)
        v.value[k] = PARAMS[k].def;
    return v;
}

/* --------------------------------------------------------------------- */
/* The engine                                                            */
/* --------------------------------------------------------------------- */

/*
 * The CLI's pipeline (bin/klattsch_cli.c), with the voice settings as
 * compiler options and the sample rate chosen: tokenize, compile, render
 * every voice and mix, encode with peak normalization and the source string
 * in the ICMT chunk. Nothing here is new engine code; it is the same calls
 * in the same order, which is what tools/verify-gui.mjs checks.
 */
struct Rendered {
    bool                       ok = false;
    std::wstring               error;
    std::string                source;     /* what was compiled */
    std::vector<unsigned char> wav;
    std::string                warnings;
    double                     seconds = 0;
    int                        sampleRate = 0;
};

static std::string ToUtf8(const std::wstring &w)
{
    if (w.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static std::wstring ToWide(const std::string &s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

/* English text to klattsch source, through csrc/kl_text.c. The contour is
 * sized from the base pitch the voice will be compiled with. */
static std::string TextToSource(const std::string &text, const Voice &v)
{
    std::unique_ptr<kl_text_ctx> ctx(new kl_text_ctx);
    kl_text_opts o;
    memset(&o, 0, sizeof o);
    o.base_f0 = v.value[0] * PARAMS[0].unit;
    o.comma_ms = v.value[P_COMMA];
    size_t need = kl_text_to_source(ctx.get(), text.c_str(), &o, NULL, 0);
    std::string out(need + 1, '\0');
    kl_text_to_source(ctx.get(), text.c_str(), &o, &out[0], out.size());
    out.resize(need);
    return out;
}

template <typename T>
static T *Alloc(std::vector<T> &v, size_t n)
{
    v.assign(n ? n : 1, T());
    return v.data();
}

static Rendered RenderSource(const std::string &source, const Voice &v)
{
    Rendered r;
    r.source = source;
    r.sampleRate = v.sampleRate;

    size_t need_src, need_tok, need_arena;
    kl_token_list L;
    std::vector<uint16_t> srcBuf;
    std::vector<kl_token> tokBuf;
    std::vector<char> arenaBuf;

    kl_token_need(source.size(), &need_src, &need_tok, &need_arena);
    memset(&L, 0, sizeof L);
    L.source = Alloc(srcBuf, need_src);   L.source_cap = need_src;
    L.tokens = Alloc(tokBuf, need_tok);   L.tokens_cap = need_tok;
    L.arena  = Alloc(arenaBuf, need_arena); L.arena_cap = need_arena;
    if (kl_tokenize(source.c_str(), source.size(), &L) != KL_TOKEN_OK) {
        r.error = L"The input is too long.";
        return r;
    }

    kl_compile_opts opts;
    memset(&opts, 0, sizeof opts);
    for (int k = 0; k < PARAM_COUNT; k++) {
        if (PARAMS[k].opt == KL_OPT_COUNT)
            continue;   /* a front-end setting, already applied */
        opts.present |= 1u << PARAMS[k].opt;
        opts.value[PARAMS[k].opt] = v.value[k] * PARAMS[k].unit;
    }
    opts.bank = kl_banks[v.bank].name;

    kl_compile_arena A;
    std::vector<kl_voice> voices;
    std::vector<kl_event> events;
    std::vector<kl_extra_span> spans;
    std::vector<kl_extra> extras;
    std::vector<kl_phrase> phrases;
    std::vector<kl_warning> warnings;
    std::vector<char> text;
    std::vector<kl_extra> live;
    std::vector<const kl_token *> syl;

    kl_compile_need(&L, &opts, &A);
    A.voices   = Alloc(voices, A.voices_cap);
    A.events   = Alloc(events, A.events_cap);
    A.spans    = Alloc(spans, A.events_cap);
    A.extras   = Alloc(extras, A.extras_cap);
    A.phrases  = Alloc(phrases, A.phrases_cap);
    A.warnings = Alloc(warnings, A.warnings_cap);
    A.text     = Alloc(text, A.text_cap);
    A.live     = Alloc(live, A.live_cap);
    A.syl      = Alloc(syl, A.syl_cap);

    kl_compiled compiled;
    if (kl_compile(&L, &opts, &A, &compiled) != KL_COMPILE_OK) {
        r.error = L"The phoneme source did not compile.";
        return r;
    }

    for (size_t vi = 0; vi < compiled.n_voices; vi++) {
        const kl_voice *cv = &compiled.voices[vi];
        for (size_t k = 0; k < cv->n_warnings; k++) {
            if (!r.warnings.empty())
                r.warnings += ", ";
            r.warnings.append(cv->warnings[k].text, cv->warnings[k].len);
        }
    }

    double sr = (double)v.sampleRate;
    size_t mix_n = kl_render_samples_for(compiled.total_ms, sr);
    size_t sched_cap = kl_render_need_sched(&compiled);
    std::vector<float> mix, scratch;
    std::vector<long> atSample, transLen;
    kl_render_arena R;
    memset(&R, 0, sizeof R);
    Alloc(mix, mix_n);
    R.scratch = Alloc(scratch, mix_n);
    R.scratch_cap = mix_n;
    R.at_sample = Alloc(atSample, sched_cap);
    R.transition_len = Alloc(transLen, sched_cap);
    R.sched_cap = sched_cap;
    if (kl_render_mix(&compiled, sr, mix.data(), mix_n, &R) != KL_RENDER_OK) {
        r.error = L"Rendering failed.";
        return r;
    }

    kl_wav_meta meta;
    meta.software = KL_WAV_SOFTWARE;
    meta.comment = source.c_str();
    size_t cap = kl_wav_size(mix_n, &meta);
    r.wav.assign(cap, 0);
    size_t len = kl_wav_encode(mix.data(), mix_n, (uint32_t)v.sampleRate,
                               KL_WAV_PEAK_NORMALIZE, &meta, r.wav.data(), cap, NULL);
    if (len == 0) {
        r.error = L"Encoding the WAV failed.";
        return r;
    }
    r.wav.resize(len);
    r.seconds = compiled.total_ms / 1000.0;
    r.ok = true;
    return r;
}

/* Text or source, as the phoneme-mode checkbox says, all the way to WAV. */
static Rendered Render(const std::wstring &input, bool phonemeMode, const Voice &v)
{
    std::string utf8 = ToUtf8(input);
    std::string source = phonemeMode ? utf8 : TextToSource(utf8, v);
    return RenderSource(source, v);
}

static bool WriteWholeFile(const wchar_t *path, const std::vector<unsigned char> &data)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    if (f == INVALID_HANDLE_VALUE)
        return false;
    BOOL ok = WriteFile(f, data.data(), (DWORD)data.size(), &written, NULL);
    CloseHandle(f);
    return ok && written == data.size();
}

/* --------------------------------------------------------------------- */
/* The window                                                            */
/* --------------------------------------------------------------------- */

enum {
    IDC_TEXTLABEL = 100, IDC_TEXT, IDC_PHONEMEMODE,
    IDC_BANKLABEL, IDC_BANK, IDC_RATELABEL, IDC_SAMPLERATE,
    IDC_SPEAK, IDC_STOP, IDC_CONVERT, IDC_SAVE, IDC_DEFAULTS,
    IDC_MSGLABEL, IDC_MESSAGES,
    IDC_PARAM_LABEL = 200, IDC_PARAM_EDIT = 300, IDC_PARAM_SPIN = 400,
};

#define WM_APP_RENDERED (WM_APP + 1)

/* "Klattsch Native 0.5.0 beta - sample generator": the version a tester
 * reports is in the title, where a screen reader reads it on focus. */
#define KL_WIDEN2(s) L##s
#define KL_WIDEN(s) KL_WIDEN2(s)
static const wchar_t *WINDOW_TITLE =
    L"Klattsch Native " KL_WIDEN(KL_VERSION_DISPLAY) L" - sample generator";
static const wchar_t *DEFAULT_TEXT =
    L"Hello. This is klattsch, a formant synthesizer. Can you hear me?";
static const wchar_t *LABEL_TEXT = L"&Text to speak:";
static const wchar_t *LABEL_SOURCE = L"&Text to speak (klattsch phoneme source):";

static HINSTANCE g_inst;
static HWND g_main, g_textLabel, g_text, g_phonemeMode, g_bank, g_sampleRate;
static HWND g_edit[PARAM_COUNT], g_spin[PARAM_COUNT];
static HWND g_speak, g_messages;
static HFONT g_font;

/*
 * Playback. Every PlaySound call is made on the UI thread; the worker only
 * renders. A render is tagged with the generation it was started in, and a
 * result from an older generation -- Speak pressed again, or Stop, while it
 * was rendering -- is dropped rather than played. The buffer being played
 * lives in g_playing until playback is stopped, since SND_ASYNC reads it as
 * it goes.
 */
static LONG g_generation;
static std::unique_ptr<Rendered> g_playing;

struct Job {
    std::wstring input;
    bool         phonemeMode;
    Voice        voice;
    LONG         generation;
};

struct Done {
    Rendered r;
    LONG     generation;
};

static std::wstring GetText(HWND h)
{
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

static bool IsChecked(HWND h)
{
    return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

/* The up-down is the authority, not the edit beside it: with UDS_SETBUDDYINT
 * it parses the buddy's text and clamps it to the range, so its position
 * follows typing and the arrow keys alike and cannot be out of range. */
static Voice VoiceFromUI(void)
{
    Voice v;
    LRESULT b = SendMessageW(g_bank, CB_GETCURSEL, 0, 0);
    LRESULT s = SendMessageW(g_sampleRate, CB_GETCURSEL, 0, 0);
    v.bank = b < 0 ? 0 : (int)b;
    v.sampleRate = SAMPLE_RATES[s < 0 ? DEFAULT_RATE_INDEX : (int)s];
    for (int k = 0; k < PARAM_COUNT; k++)
        v.value[k] = (int)SendMessageW(g_spin[k], UDM_GETPOS32, 0, 0);
    return v;
}

static void ApplyVoice(const Voice &v)
{
    SendMessageW(g_bank, CB_SETCURSEL, (WPARAM)v.bank, 0);
    for (int i = 0; i < RATE_COUNT; i++)
        if (SAMPLE_RATES[i] == v.sampleRate)
            SendMessageW(g_sampleRate, CB_SETCURSEL, (WPARAM)i, 0);
    for (int k = 0; k < PARAM_COUNT; k++)
        SendMessageW(g_spin[k], UDM_SETPOS32, 0, v.value[k]);
}

/* Messages go into a read-only edit rather than a message box, so they can
 * be read again, line by line, at leisure -- and so speaking does not steal
 * focus. The edit is in the tab order for that reason. */
static void SetMessages(const std::wstring &s)
{
    SetWindowTextW(g_messages, s.c_str());
}

static std::wstring Describe(const Rendered &r, bool phonemeMode)
{
    wchar_t head[128];
    std::wstring m;
    if (!r.ok)
        return L"Error: " + r.error;
    _snwprintf(head, 127, L"%.2f seconds at %d Hz.", r.seconds, r.sampleRate);
    head[127] = L'\0';
    m = head;
    if (!r.warnings.empty())
        m += L"\r\nWarnings: " + ToWide(r.warnings);
    if (!phonemeMode)
        m += L"\r\nPhoneme source: " + ToWide(r.source);
    return m;
}

static void StopPlayback(void)
{
    InterlockedIncrement(&g_generation);   /* drop any render in flight */
    PlaySoundW(NULL, NULL, 0);
    g_playing.reset();
}

static DWORD WINAPI RenderThread(LPVOID param)
{
    std::unique_ptr<Job> job((Job *)param);
    Done *d = new Done;
    d->r = Render(job->input, job->phonemeMode, job->voice);
    d->generation = job->generation;
    if (!PostMessageW(g_main, WM_APP_RENDERED, 0, (LPARAM)d))
        delete d;
    return 0;
}

static void OnSpeak(void)
{
    std::wstring input = GetText(g_text);
    if (input.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
        SetMessages(L"Nothing to speak: the text box is empty.");
        return;
    }
    StopPlayback();
    Job *job = new Job;
    job->input = input;
    job->phonemeMode = IsChecked(g_phonemeMode);
    job->voice = VoiceFromUI();   /* read on the UI thread */
    job->generation = g_generation;
    SetMessages(L"Rendering...");
    HANDLE th = CreateThread(NULL, 0, RenderThread, job, 0, NULL);
    if (th == NULL) {
        delete job;
        SetMessages(L"Error: could not start rendering.");
        return;
    }
    CloseHandle(th);
}

static void OnRendered(Done *d)
{
    std::unique_ptr<Done> done(d);
    if (done->generation != g_generation)
        return;   /* stopped, or superseded by a newer Speak */
    SetMessages(Describe(done->r, IsChecked(g_phonemeMode)));
    if (!done->r.ok)
        return;
    g_playing.reset(new Rendered(std::move(done->r)));
    PlaySoundW((LPCWSTR)g_playing->wav.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

static void SetPhonemeMode(bool on)
{
    SendMessageW(g_phonemeMode, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(g_textLabel, on ? LABEL_SOURCE : LABEL_TEXT);
}

/* Text to phoneme source, in place: the way to see, and edit, what the front
 * end made of a sentence before it is spoken. */
static void OnConvert(void)
{
    if (IsChecked(g_phonemeMode)) {
        SetMessages(L"The box already holds phoneme source. Clear phoneme "
                    L"mode to convert text.");
        return;
    }
    std::wstring input = GetText(g_text);
    std::string src = TextToSource(ToUtf8(input), VoiceFromUI());
    SetWindowTextW(g_text, ToWide(src).c_str());
    SetPhonemeMode(true);
    SetMessages(L"Converted. The box now holds phoneme source, and phoneme "
                L"mode is on.");
    SetFocus(g_text);
}

static void OnSave(void)
{
    std::wstring input = GetText(g_text);
    wchar_t path[MAX_PATH] = L"klattsch.wav";
    OPENFILENAMEW ofn;

    if (input.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
        SetMessages(L"Nothing to save: the text box is empty.");
        return;
    }
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"WAV files (*.wav)\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Save WAV file";
    ofn.lpstrDefExt = L"wav";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn))
        return;

    bool phonemeMode = IsChecked(g_phonemeMode);
    Rendered r = Render(input, phonemeMode, VoiceFromUI());
    if (!r.ok) {
        SetMessages(Describe(r, phonemeMode));
        return;
    }
    if (!WriteWholeFile(path, r.wav)) {
        SetMessages(std::wstring(L"Error: could not write ") + path);
        return;
    }
    SetMessages(std::wstring(L"Saved ") + path + L".\r\n" + Describe(r, phonemeMode));
}

/*
 * A multi-line EDIT answers WM_GETDLGCODE with DLGC_WANTALLKEYS, and
 * IsDialogMessage then hands it Tab, which it inserts as a character: focus
 * goes in and cannot get out. That is a keyboard trap, in a program whose
 * users are the people least able to reach for a mouse instead. So the edit
 * stands aside for a Tab keydown, and the dialog manager moves focus. (The
 * same fix, and the same reasoning, as Votraxxion's gui-native.)
 *
 * And for Escape. Given Escape, a multi-line edit posts WM_CLOSE to its
 * parent -- documented EDIT behaviour -- and this window closed, taking the
 * text with it, whenever Escape was pressed in either box. Found by
 * tools/check-gui-a11y.ps1 on its first run. Standing aside lets
 * IsDialogMessage turn Escape into IDCANCEL, which stops speech.
 */
static WNDPROC g_editProc;

static LRESULT CALLBACK NoTabProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_GETDLGCODE) {
        LRESULT code = CallWindowProcW(g_editProc, h, msg, wp, lp);
        const MSG *m = (const MSG *)lp;
        if (m != NULL && m->message == WM_KEYDOWN &&
            (m->wParam == VK_TAB || m->wParam == VK_ESCAPE))
            code &= ~(LRESULT)(DLGC_WANTALLKEYS | DLGC_WANTTAB);
        return code;
    }
    return CallWindowProcW(g_editProc, h, msg, wp, lp);
}

static HWND Make(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex,
                 int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, g_main, (HMENU)(INT_PTR)id, g_inst, NULL);
    if (c != NULL)
        SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static HWND MakeMultiline(const wchar_t *text, DWORD extra, int x, int y, int w,
                          int h, int id)
{
    HWND c = Make(L"EDIT", text, WS_TABSTOP | WS_VSCROLL | ES_MULTILINE
                  | ES_AUTOVSCROLL | extra, WS_EX_CLIENTEDGE, x, y, w, h, id);
    WNDPROC old = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(c, GWLP_WNDPROC,
                                                      (LONG_PTR)NoTabProc);
    if (g_editProc == NULL)
        g_editProc = old;   /* both are the system EDIT procedure */
    return c;
}

#define CLIENT_W 640

static int CreateControls(void)
{
    const int M = 12, LBL = 20, ROW = 30, GAP = 8;
    const int W = CLIENT_W - 2 * M;
    const int colW = (W - GAP) / 2, labW = 170, edW = 80;
    int y = M;

    /* Each label is created immediately before the control it names, so it
     * precedes it in z-order -- which is the tab order and the order a
     * screen reader reads. */
    g_textLabel = Make(L"STATIC", LABEL_TEXT, 0, 0, M, y, W, LBL, IDC_TEXTLABEL);
    y += LBL + 2;
    g_text = MakeMultiline(DEFAULT_TEXT, ES_WANTRETURN, M, y, W, 110, IDC_TEXT);
    y += 110 + GAP;

    g_phonemeMode = Make(L"BUTTON", L"P&honeme mode (the box holds klattsch phoneme source)",
                         BS_AUTOCHECKBOX | WS_TABSTOP, 0, M, y, W, LBL, IDC_PHONEMEMODE);
    y += LBL + GAP;

    Make(L"STATIC", L"Phoneme &bank:", SS_RIGHT, 0, M, y + 4, labW, LBL, IDC_BANKLABEL);
    g_bank = Make(L"COMBOBOX", NULL, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0,
                  M + labW + GAP, y, 260, 200, IDC_BANK);
    for (size_t i = 0; i < kl_bank_count; i++) {
        std::wstring name = ToWide(kl_banks[i].display_name ? kl_banks[i].display_name
                                                           : kl_banks[i].name);
        SendMessageW(g_bank, CB_ADDSTRING, 0, (LPARAM)name.c_str());
    }
    y += ROW;

    Make(L"STATIC", L"Sample rate (Hz):", SS_RIGHT, 0, M, y + 4, labW, LBL, IDC_RATELABEL);
    g_sampleRate = Make(L"COMBOBOX", NULL, WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0,
                        M + labW + GAP, y, 120, 200, IDC_SAMPLERATE);
    for (int i = 0; i < RATE_COUNT; i++) {
        wchar_t buf[16];
        _snwprintf(buf, 15, L"%d", SAMPLE_RATES[i]);
        buf[15] = L'\0';
        SendMessageW(g_sampleRate, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    y += ROW + GAP;

    /* Two columns, filled column by column so that the tab order
     * reads down the first and then down the second. */
    {
        const int top = y, half = (PARAM_COUNT + 1) / 2;
        for (int k = 0; k < PARAM_COUNT; k++) {
            const ParamSpec *p = &PARAMS[k];
            int col = k < half ? 0 : 1;
            int row = k < half ? k : k - half;
            int x = M + col * (colW + GAP);
            int yy = top + row * ROW;
            wchar_t buf[16];
            Make(L"STATIC", p->label, SS_RIGHT, 0, x, yy + 4, labW, LBL, IDC_PARAM_LABEL + k);
            _snwprintf(buf, 15, L"%d", p->def);
            buf[15] = L'\0';
            /* No ES_NUMBER: it refuses the minus sign tilt needs. The
             * up-down parses and clamps whatever is typed. */
            g_edit[k] = Make(L"EDIT", buf, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
                             x + labW + GAP, yy, edW, 24, IDC_PARAM_EDIT + k);
            g_spin[k] = CreateWindowExW(0, UPDOWN_CLASSW, NULL,
                                        WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT
                                        | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                                        0, 0, 0, 0, g_main,
                                        (HMENU)(INT_PTR)(IDC_PARAM_SPIN + k), g_inst, NULL);
            SendMessageW(g_spin[k], UDM_SETBUDDY, (WPARAM)g_edit[k], 0);
            SendMessageW(g_spin[k], UDM_SETRANGE32, (WPARAM)p->lo, (LPARAM)p->hi);
            SendMessageW(g_spin[k], UDM_SETPOS32, 0, p->def);
        }
        y = top + half * ROW + GAP;
    }

    {
        const int bh = 28, bg = 8;
        int bx = M;
        g_speak = Make(L"BUTTON", L"Spea&k", BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
                       bx, y, 90, bh, IDC_SPEAK);
        bx += 90 + bg;
        Make(L"BUTTON", L"St&op", BS_PUSHBUTTON | WS_TABSTOP, 0, bx, y, 80, bh, IDC_STOP);
        bx += 80 + bg;
        Make(L"BUTTON", L"&Convert to phonemes", BS_PUSHBUTTON | WS_TABSTOP, 0,
             bx, y, 170, bh, IDC_CONVERT);
        bx += 170 + bg;
        Make(L"BUTTON", L"Save &WAV...", BS_PUSHBUTTON | WS_TABSTOP, 0, bx, y, 110, bh, IDC_SAVE);
        bx += 110 + bg;
        Make(L"BUTTON", L"Reset &defaults", BS_PUSHBUTTON | WS_TABSTOP, 0,
             bx, y, 130, bh, IDC_DEFAULTS);
        y += bh + GAP;
    }

    Make(L"STATIC", L"&Messages:", 0, 0, M, y, W, LBL, IDC_MSGLABEL);
    y += LBL + 2;
    g_messages = MakeMultiline(L"Ready. Press Speak (Alt+K), or Escape to stop.",
                               ES_READONLY, M, y, W, 90, IDC_MESSAGES);
    y += 90 + M;

    ApplyVoice(DefaultVoice());
    return y;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_main = hwnd;
        CreateControls();
        SetFocus(g_text);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SPEAK:    OnSpeak();   return 0;
        case IDC_STOP:     StopPlayback(); SetMessages(L"Stopped."); return 0;
        case IDC_CONVERT:  OnConvert(); return 0;
        case IDC_SAVE:     OnSave();    return 0;
        case IDC_DEFAULTS:
            ApplyVoice(DefaultVoice());
            SetMessages(L"All voice settings are back at their defaults.");
            return 0;
        case IDC_PHONEMEMODE:
            if (HIWORD(wp) == BN_CLICKED)
                SetPhonemeMode(IsChecked(g_phonemeMode));
            return 0;
        /* Escape arrives as IDCANCEL from IsDialogMessage. It stops speech
         * rather than closing the window: in a program you listen to, the
         * key you reach for mid-sentence should not throw your work away.
         * Alt+F4 closes. */
        case IDCANCEL:
            StopPlayback();
            SetMessages(L"Stopped.");
            return 0;
        }
        break;

    case WM_APP_RENDERED:
        OnRendered((Done *)lp);
        return 0;

    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_messages)
            break;   /* keep the read-only edit's own background */
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_CLOSE:
        StopPlayback();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --------------------------------------------------------------------- */
/* Headless self-test                                                    */
/* --------------------------------------------------------------------- */

/*
 *   klattsch_gui.exe --selftest OUT.WAV PHONEMEMODE BANK SAMPLERATE
 *                               P1 ... P11 TEXT
 *
 * PHONEMEMODE is 0 or 1 and means what the checkbox means; BANK is a bank
 * name; P1..P11 are the eleven spin-box values in the order of PARAMS, in the
 * units the boxes show. Everything downstream is the code the buttons run --
 * Render(), TextToSource(), RenderSource() -- so a match in
 * tools/verify-gui.mjs is a statement about this executable, not about a copy
 * of its logic. Returns 0 on success.
 */
static int RunSelfTest(int argc, wchar_t **argv)
{
    if (argc != 7 + PARAM_COUNT)
        return 2;
    Voice v = DefaultVoice();
    std::string bank = ToUtf8(argv[4]);
    bool found = false;
    for (size_t i = 0; i < kl_bank_count; i++) {
        if (bank == kl_banks[i].name) {
            v.bank = (int)i;
            found = true;
        }
    }
    if (!found)
        return 3;
    v.sampleRate = _wtoi(argv[5]);
    for (int k = 0; k < PARAM_COUNT; k++)
        v.value[k] = _wtoi(argv[6 + k]);
    Rendered r = Render(argv[6 + PARAM_COUNT], _wtoi(argv[3]) != 0, v);
    if (!r.ok)
        return 4;
    return WriteWholeFile(argv[2], r.wav) ? 0 : 5;
}

static void MakeFont(void)
{
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (g_font == NULL)
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show)
{
    g_inst = inst;

    {
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv != NULL && argc >= 2 && wcscmp(argv[1], L"--selftest") == 0) {
            int rc = RunSelfTest(argc, argv);
            LocalFree(argv);
            return rc;
        }
        if (argv != NULL)
            LocalFree(argv);
    }

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    MakeFont();

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"KlattschGeneratorWindow";
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wc))
        return 1;

    /* The height is what CreateControls laid out; a first window is created
     * at a provisional size and resized once the controls exist. */
    DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, WINDOW_TITLE, style,
                                CW_USEDEFAULT, CW_USEDEFAULT, 700, 700,
                                NULL, NULL, inst, NULL);
    if (hwnd == NULL)
        return 1;
    {
        RECT last, r;
        GetWindowRect(g_messages, &last);
        MapWindowPoints(NULL, hwnd, (POINT *)&last, 2);
        r.left = 0;
        r.top = 0;
        r.right = CLIENT_W;
        r.bottom = last.bottom + 12;
        AdjustWindowRect(&r, style, FALSE);
        SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER);
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* IsDialogMessage gives tab order, the & accelerators, Escape and
         * the default button -- everything a keyboard user needs and none of
         * which a bare message loop provides. */
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
