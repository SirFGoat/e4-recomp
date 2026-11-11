// e4 - game data patches applied after the XEX is loaded.
#pragma once

#include <cstdint>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

// Only included from main.cpp, so the cvar is defined once.
REXCVAR_DEFINE_BOOL(e4_text_patches, true, "E4", "Rename 'Return to Arcade' to 'Quit'");

namespace e4 {

extern rex::memory::Memory* g_memory;  // defined in e4_particles.cpp

// The game's strings are big-endian UTF-16 in its read-only data. Each patch
// finds a whole null-terminated string by its exact text and overwrites it
// with a shorter one, zero-filling the rest so neighbouring strings are untouched.
struct TextPatch {
  std::u16string_view from;
  std::u16string_view to;
};

// "Return to Arcade" exits the game in this recomp, so call it Quit.
inline constexpr TextPatch kTextPatches[] = {
    // Menu item (English, French, Italian, German, Spanish, Portuguese).
    {u"Return To Arcade", u"Quit"},
    {u"Retour vers Arcade", u"Quitter"},
    {u"Torna ad Arcade", u"Esci"},
    {u"Zurück zu Arcade", u"Beenden"},
    {u"Volver a Arcade", u"Salir"},
    {u"Voltar ao Arcade", u"Sair"},
    // Help line under the menu item.
    {u"Press [A] to exit to Xbox LIVE Arcade.  [B] Back", u"Press [A] to quit the game.  [B] Back"},
    {u"[A]　Xbox LIVE Arcade に戻ります。  [B] メニューに戻ります。",
     u"[A]　ゲームを終了します。  [B] メニューに戻ります。"},
    {u"Appuyer sur [A] pour quitter Xbox LIVE Arcade.  [B] Retour", u"Appuyer sur [A] pour quitter le jeu.  [B] Retour"},
    {u"Premi [A] per tornare a Xbox LIVE Arcade.  [B] Indietro", u"Premi [A] per uscire dal gioco.  [B] Indietro"},
    {u"Mit [A] zurück zu Xbox LIVE Arcade.  [B] Zurück", u"Mit [A] das Spiel beenden.  [B] Zurück"},
    {u"Pulsa [A] para salir a Xbox LIVE Arcade.  [B] Atrás", u"Pulsa [A] para salir del juego.  [B] Atrás"},
    {u"Prime [A] para saíres para o Xbox LIVE Arcade.  [B] Retroceder", u"Prime [A] para saíres do jogo.  [B] Retroceder"},
    // Confirmation dialog.
    {u"Are you sure you want to exit to Xbox LIVE Arcade?", u"Are you sure you want to quit?"},
    {u"Voulez-vous vraiment quitter et retourner vers Xbox LIVE Arcade ?", u"Voulez-vous vraiment quitter le jeu ?"},
    {u"Sei sicuro di voler uscire da Xbox LIVE Arcade?", u"Sei sicuro di voler uscire dal gioco?"},
    {u"Wollen Sie wirklich zu Xbox LIVE Arcade wechseln?", u"Wollen Sie das Spiel wirklich beenden?"},
    {u"¿Seguro que quieres salir de Xbox LIVE Arcade?", u"¿Seguro que quieres salir del juego?"},
    {u"Tens a certeza de que queres sair do Xbox LIVE Arcade?", u"Tens a certeza de que queres sair do jogo?"},
    {u"ゲームを終了し、Xbox LIVE アーケードに戻ります。",
     u"ゲームを終了します。"},
    {u"Xbox LIVE アーケードに戻りますか？",
     u"ゲームを終了しますか？"},
};

// .rdata, where all of the strings above live.
inline constexpr uint32_t kStringScanBegin = 0x82000000;
inline constexpr uint32_t kStringScanEnd = 0x82240000;

inline bool MatchesBE(const uint8_t* p, std::u16string_view s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (p[i * 2] != uint8_t(s[i] >> 8) || p[i * 2 + 1] != uint8_t(s[i])) return false;
  }
  return true;
}

inline void ApplyTextPatches(rex::memory::Memory& memory) {
  if (!REXCVAR_GET(e4_text_patches)) return;
  uint8_t* base = memory.TranslateVirtual(kStringScanBegin);
  const size_t size = kStringScanEnd - kStringScanBegin;

  // .rdata is mapped read-only: unlock it for the patch, then restore.
  auto* heap = memory.LookupHeap(kStringScanBegin);
  uint32_t old_protect = rex::memory::kMemoryProtectRead;
  if (!heap || !heap->Protect(kStringScanBegin, uint32_t(size),
                              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                              &old_protect)) {
    REXLOG_ERROR("e4 text patch: could not unprotect .rdata, skipped");
    return;
  }
  for (const auto& patch : kTextPatches) {
    if (patch.to.size() > patch.from.size()) {
      REXLOG_ERROR("e4 text patch: replacement longer than original, skipped");
      continue;
    }
    const size_t len = patch.from.size() * 2;
    int count = 0;
    for (size_t off = 2; off + len + 2 <= size; off += 2) {
      uint8_t* p = base + off;
      // Whole string only: a null character before and after.
      if (p[-2] || p[-1] || p[len] || p[len + 1] || !MatchesBE(p, patch.from)) continue;
      for (size_t i = 0; i < patch.from.size(); ++i) {
        char16_t c = i < patch.to.size() ? patch.to[i] : u'\0';
        p[i * 2] = uint8_t(c >> 8);
        p[i * 2 + 1] = uint8_t(c);
      }
      ++count;
    }
    if (count == 0) {
      REXLOG_WARN("e4 text patch: string not found ({} chars)", patch.from.size());
    }
  }
  heap->Protect(kStringScanBegin, uint32_t(size), old_protect);
  REXLOG_INFO("e4 text patches applied");
}

}  // namespace e4
