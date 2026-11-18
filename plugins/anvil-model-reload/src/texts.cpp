// The window's words: English, then French, German, Spanish, Simplified and
// Traditional Chinese, Japanese, Korean and Polish. Drafted by the agent; each
// language waits for a native speaker's check (docs/model-reload.md).
#include "texts.hpp"

#include <cstring>

namespace model_reload {

namespace {

const TextEntry kEntries[] = {
    {"Model reload",
     {"Rechargement des modèles", "Modelle neu laden", "Recarga de modelos", "模型重载", "模型重新載入",
      "モデルの再読み込み", "모델 다시 불러오기", "Przeładowanie modeli"}},
    {"For model modders: reloads the meshes, model textures, animations, entities and .gfx mesh entries you saved, "
     "in the running game.",
     {"Pour les moddeurs de modèles : recharge dans la partie en cours les maillages, textures de modèles, "
      "animations, entités et entrées de maillage .gfx que vous avez enregistrés.",
      "Für Modder von Modellen: lädt die gespeicherten Meshes, Modelltexturen, Animationen, Entitäten und "
      ".gfx-Mesh-Einträge im laufenden Spiel neu.",
      "Para modders de modelos: recarga en la partida en curso las mallas, texturas de modelos, animaciones, "
      "entidades y entradas de malla .gfx que guardaste.",
      "面向模型模组作者：在运行中的游戏里重新载入你保存的网格、模型纹理、动画、实体和 .gfx 网格条目。",
      "給模型模組作者：在執行中的遊戲裡重新載入你儲存的網格、模型材質、動畫、實體和 .gfx 網格條目。",
      "モデルを作るモッダー向け：保存したメッシュ、モデルのテクスチャ、アニメーション、エンティティ、.gfx "
      "のメッシュ項目を、実行中のゲームで再読み込みします。",
      "모델 모더용: 저장한 메시, 모델 텍스처, 애니메이션, 엔티티, .gfx 메시 항목을 실행 중인 게임에서 다시 "
      "불러옵니다.",
      "Dla modderów modeli: wczytuje ponownie w działającej grze zapisane siatki, tekstury modeli, animacje, encje "
      "i wpisy siatek w plikach .gfx."}},
    {"Mods watched",
     {"Mods surveillés", "Beobachtete Mods", "Mods vigilados", "监视的模组", "監看的模組", "監視中のMOD",
      "감시 중인 모드", "Obserwowane mody"}},
    {"none", {"aucun", "keine", "ninguno", "无", "無", "なし", "없음", "brak"}},
    {"{0} enabled mods cannot be watched; anvil.log names them.",
     {"{0} mods activés ne peuvent pas être surveillés ; anvil.log les nomme.",
      "{0} aktivierte Mods können nicht beobachtet werden; anvil.log nennt sie.",
      "{0} mods activados no se pueden vigilar; anvil.log los nombra.", "{0} 个已启用的模组无法监视；anvil.log 列出了它们。",
      "{0} 個已啟用的模組無法監看；anvil.log 列出了它們。", "有効なMODのうち {0} 個は監視できません。anvil.log に名前があります。",
      "활성화된 모드 {0}개를 감시할 수 없습니다. anvil.log에 이름이 있습니다.",
      "Włączonych modów, których nie da się obserwować: {0}; anvil.log je wymienia."}},
    {"More than {0} files: only the first are watched.",
     {"Plus de {0} fichiers : seuls les premiers sont surveillés.",
      "Mehr als {0} Dateien: nur die ersten werden beobachtet.",
      "Más de {0} archivos: solo se vigilan los primeros.", "超过 {0} 个文件：只监视前面的部分。",
      "超過 {0} 個檔案：只監看前面的部分。", "ファイルが {0} 個を超えています。最初の分だけを監視します。",
      "파일이 {0}개를 넘습니다. 앞쪽 파일만 감시합니다.", "Ponad {0} plików: obserwowane są tylko pierwsze."}},
    {"Looking at the mods' files...",
     {"Examen des fichiers des mods...", "Die Dateien der Mods werden gelesen...", "Revisando los archivos de los mods...",
      "正在查看模组的文件……", "正在查看模組的檔案……", "MODのファイルを確認しています…", "모드 파일을 살펴보는 중...",
      "Przeglądanie plików modów..."}},
    {"No file changed since the last reload.",
     {"Aucun fichier modifié depuis le dernier rechargement.", "Seit dem letzten Neuladen hat sich keine Datei geändert.",
      "Ningún archivo cambió desde la última recarga.", "自上次重载以来没有文件变化。", "自上次重新載入以來沒有檔案變更。",
      "前回の再読み込みから変更されたファイルはありません。", "마지막으로 다시 불러온 뒤 바뀐 파일이 없습니다.",
      "Od ostatniego przeładowania nie zmienił się żaden plik."}},
    {"Changed since the last reload",
     {"Modifiés depuis le dernier rechargement", "Seit dem letzten Neuladen geändert",
      "Cambiados desde la última recarga", "自上次重载以来的变化", "自上次重新載入以來的變更",
      "前回の再読み込みからの変更", "마지막으로 다시 불러온 뒤 바뀐 파일", "Zmienione od ostatniego przeładowania"}},
    {"{0} more",
     {"{0} de plus", "{0} weitere", "{0} más", "另有 {0} 个", "另有 {0} 個", "ほか {0} 件", "{0}개 더", "i {0} więcej"}},
    {"Load a game to reload.",
     {"Chargez une partie pour recharger.", "Lade ein Spiel, um neu zu laden.", "Carga una partida para recargar.",
      "载入游戏后才能重载。", "載入遊戲後才能重新載入。", "再読み込みするにはゲームを読み込んでください。",
      "다시 불러오려면 게임을 불러오세요.", "Wczytaj grę, żeby przeładować."}},
    {"Not in ironman or multiplayer games.",
     {"Pas dans les parties ironman ou multijoueur.", "Nicht in Ironman- oder Mehrspielerpartien.",
      "No en partidas ironman ni multijugador.", "不能在铁人或多人游戏中使用。", "不能在鐵人或多人遊戲中使用。",
      "アイアンマンとマルチプレイのゲームでは使えません。", "아이언맨이나 멀티플레이 게임에서는 쓸 수 없습니다.",
      "Nie w grach ironman ani wieloosobowych."}},
    {"Reloading...",
     {"Rechargement...", "Wird neu geladen...", "Recargando...", "正在重载……", "正在重新載入……", "再読み込み中…",
      "다시 불러오는 중...", "Przeładowywanie..."}},
    {"Reload the changed files",
     {"Recharger les fichiers modifiés", "Geänderte Dateien neu laden", "Recargar los archivos cambiados",
      "重载变化的文件", "重新載入變更的檔案", "変更したファイルを再読み込み", "바뀐 파일 다시 불러오기",
      "Przeładuj zmienione pliki"}},
    {"Last reload",
     {"Dernier rechargement", "Letztes Neuladen", "Última recarga", "上次重载", "上次重新載入", "前回の再読み込み",
      "마지막 다시 불러오기", "Ostatnie przeładowanie"}},
    {"Not reloaded: {0}",
     {"Non rechargé : {0}", "Nicht neu geladen: {0}", "No recargado: {0}", "未重载：{0}", "未重新載入：{0}",
      "再読み込みされない項目：{0}", "다시 불러오지 않음: {0}", "Nie przeładowano: {0}"}},
    {"Units on the map are drawn again with the reloaded entities ({0} models).",
     {"Les unités sur la carte sont redessinées avec les entités rechargées ({0} modèles).",
      "Die Einheiten auf der Karte werden mit den neu geladenen Entitäten neu gezeichnet ({0} Modelle).",
      "Las unidades del mapa se vuelven a dibujar con las entidades recargadas ({0} modelos).",
      "地图上的单位已用重载的实体重新绘制（{0} 个模型）。", "地圖上的單位已用重新載入的實體重新繪製（{0} 個模型）。",
      "マップ上のユニットは再読み込みしたエンティティで描き直されます（モデル {0} 個）。",
      "지도의 유닛을 다시 불러온 엔티티로 다시 그립니다(모델 {0}개).",
      "Jednostki na mapie są rysowane od nowa z przeładowanymi encjami (modele: {0})."}},
    {"Units already on the map keep their old entity: load a save to see the change.",
     {"Les unités déjà sur la carte gardent leur ancienne entité : chargez une sauvegarde pour voir le changement.",
      "Einheiten, die schon auf der Karte sind, behalten ihre alte Entität: lade einen Spielstand, um die Änderung zu "
      "sehen.",
      "Las unidades que ya están en el mapa conservan su entidad anterior: carga una partida guardada para ver el "
      "cambio.",
      "已在地图上的单位保留旧的实体：载入存档即可看到变化。", "已在地圖上的單位保留舊的實體：載入存檔即可看到變更。",
      "マップ上にいるユニットは古いエンティティのままです。変更を見るにはセーブを読み込んでください。",
      "이미 지도에 있는 유닛은 예전 엔티티를 유지합니다. 변경을 보려면 저장 파일을 불러오세요.",
      "Jednostki, które już są na mapie, zachowują starą encję: wczytaj zapis, żeby zobaczyć zmianę."}},
    {"{0} mesh types reloaded",
     {"{0} types de maillage rechargés", "{0} Mesh-Typen neu geladen", "{0} tipos de malla recargados",
      "已重载 {0} 个网格类型", "已重新載入 {0} 個網格類型", "メッシュ型 {0} 個を再読み込みしました",
      "메시 유형 {0}개를 다시 불러왔습니다", "Przeładowane typy siatek: {0}"}},
    {"{0} mesh types updated, {1} unchanged",
     {"{0} types de maillage mis à jour, {1} inchangés", "{0} Mesh-Typen aktualisiert, {1} unverändert",
      "{0} tipos de malla actualizados, {1} sin cambios", "已更新 {0} 个网格类型，{1} 个未变",
      "已更新 {0} 個網格類型，{1} 個未變", "メッシュ型 {0} 個を更新、{1} 個は変更なし",
      "메시 유형 {0}개 갱신, {1}개 변경 없음", "Zaktualizowane typy siatek: {0}, bez zmian: {1}"}},
    {"Done", {"Fait", "Erledigt", "Hecho", "完成", "完成", "完了", "완료", "Gotowe"}},
    {"The console answered: {0}",
     {"La console a répondu : {0}", "Die Konsole antwortete: {0}", "La consola respondió: {0}", "控制台回复：{0}",
      "主控台回覆：{0}", "コンソールの応答：{0}", "콘솔의 답: {0}", "Konsola odpowiedziała: {0}"}},
    {"The game has no console now.",
     {"Le jeu n'a pas de console pour l'instant.", "Das Spiel hat gerade keine Konsole.",
      "El juego no tiene consola ahora.", "游戏现在没有控制台。", "遊戲現在沒有主控台。", "ゲームには今コンソールがありません。",
      "지금 게임에 콘솔이 없습니다.", "Gra nie ma teraz konsoli."}},
    {"The game's mesh types cannot be read.",
     {"Les types de maillage du jeu sont illisibles.", "Die Mesh-Typen des Spiels können nicht gelesen werden.",
      "No se pueden leer los tipos de malla del juego.", "无法读取游戏的网格类型。", "無法讀取遊戲的網格類型。",
      "ゲームのメッシュ型を読み取れません。", "게임의 메시 유형을 읽을 수 없습니다.",
      "Nie da się odczytać typów siatek gry."}},
    {"No mesh type of the game uses this file.",
     {"Aucun type de maillage du jeu n'utilise ce fichier.", "Kein Mesh-Typ des Spiels verwendet diese Datei.",
      "Ningún tipo de malla del juego usa este archivo.", "游戏中没有网格类型使用此文件。", "遊戲中沒有網格類型使用此檔案。",
      "このファイルを使うメッシュ型はゲームにありません。", "이 파일을 쓰는 메시 유형이 게임에 없습니다.",
      "Żaden typ siatki w grze nie używa tego pliku."}},
    {"The file cannot be read, or its braces do not close.",
     {"Le fichier est illisible, ou ses accolades ne se ferment pas.",
      "Die Datei kann nicht gelesen werden, oder ihre Klammern schließen nicht.",
      "No se puede leer el archivo, o sus llaves no se cierran.", "无法读取该文件，或其括号没有闭合。",
      "無法讀取該檔案，或其括號沒有閉合。", "ファイルを読めないか、括弧が閉じていません。",
      "파일을 읽을 수 없거나 중괄호가 닫히지 않습니다.", "Pliku nie da się odczytać albo jego nawiasy się nie zamykają."}},
    {"An error in the game's code: the rest did not run. Save and restart the game.",
     {"Une erreur dans le code du jeu : le reste n'a pas été exécuté. Sauvegardez et redémarrez le jeu.",
      "Ein Fehler im Code des Spiels: der Rest lief nicht. Speichere und starte das Spiel neu.",
      "Un error en el código del juego: el resto no se ejecutó. Guarda y reinicia el juego.",
      "游戏代码出错：其余部分未执行。请存档并重启游戏。", "遊戲程式碼出錯：其餘部分未執行。請存檔並重新啟動遊戲。",
      "ゲームのコードでエラーが起きました。残りは実行していません。セーブしてゲームを再起動してください。",
      "게임 코드에서 오류가 났습니다. 나머지는 실행하지 않았습니다. 저장하고 게임을 다시 시작하세요.",
      "Błąd w kodzie gry: reszta się nie wykonała. Zapisz grę i uruchom ją ponownie."}},
    {"Not run",
     {"Non exécuté", "Nicht ausgeführt", "No ejecutado", "未执行", "未執行", "未実行", "실행 안 함", "Nie wykonano"}},
    {"A new mesh type: it needs a restart.",
     {"Un nouveau type de maillage : il faut redémarrer.", "Ein neuer Mesh-Typ: er braucht einen Neustart.",
      "Un tipo de malla nuevo: necesita reiniciar.", "新的网格类型：需要重启。", "新的網格類型：需要重新啟動。",
      "新しいメッシュ型です。再起動が必要です。", "새 메시 유형입니다. 다시 시작해야 합니다.",
      "Nowy typ siatki: wymaga ponownego uruchomienia."}},
    {"Its animation ids changed: it needs a restart.",
     {"Ses identifiants d'animation ont changé : il faut redémarrer.",
      "Seine Animations-IDs haben sich geändert: er braucht einen Neustart.",
      "Sus identificadores de animación cambiaron: necesita reiniciar.", "其动画 id 有变化：需要重启。",
      "其動畫 id 有變更：需要重新啟動。", "アニメーションの id が変わりました。再起動が必要です。",
      "애니메이션 id가 바뀌었습니다. 다시 시작해야 합니다.", "Zmieniły się jego id animacji: wymaga ponownego uruchomienia."}},
    {"Its file is in no mod and not in the game: left as it was.",
     {"Son fichier n'est dans aucun mod ni dans le jeu : laissé tel quel.",
      "Seine Datei ist in keinem Mod und nicht im Spiel: unverändert gelassen.",
      "Su archivo no está en ningún mod ni en el juego: se dejó como estaba.", "其文件不在任何模组中，也不在游戏中：保持原样。",
      "其檔案不在任何模組中，也不在遊戲中：保持原樣。", "そのファイルはどのMODにもゲームにもありません。元のままです。",
      "그 파일이 어떤 모드에도, 게임에도 없습니다. 그대로 두었습니다.",
      "Jego pliku nie ma w żadnym modzie ani w grze: zostawiony bez zmian."}},
    {"A value cannot be read: left as it was.",
     {"Une valeur est illisible : laissé tel quel.", "Ein Wert kann nicht gelesen werden: unverändert gelassen.",
      "No se puede leer un valor: se dejó como estaba.", "有一个值无法读取：保持原样。", "有一個值無法讀取：保持原樣。",
      "読めない値があります。元のままです。", "읽을 수 없는 값이 있습니다. 그대로 두었습니다.",
      "Nie da się odczytać wartości: zostawiony bez zmian."}},
    {"An error in the game's code.",
     {"Une erreur dans le code du jeu.", "Ein Fehler im Code des Spiels.", "Un error en el código del juego.",
      "游戏代码出错。", "遊戲程式碼出錯。", "ゲームのコードでエラーが起きました。", "게임 코드에서 오류가 났습니다.",
      "Błąd w kodzie gry."}},
};

}  // namespace

const TextEntry* text_entries(std::size_t& count) {
    count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

Lang lang_of(std::string_view code) {
    if (code == "fr") return Lang::fr;
    if (code == "de") return Lang::de;
    if (code == "es") return Lang::es;
    if (code == "zh-Hans") return Lang::zh_hans;
    if (code == "zh-Hant") return Lang::zh_hant;
    if (code == "ja") return Lang::ja;
    if (code == "ko") return Lang::ko;
    if (code == "pl") return Lang::pl;
    return Lang::en;
}

const char* Texts::operator()(const char* english) const {
    if (lang == Lang::en) return english;
    for (const TextEntry& e : kEntries) {
        if (std::strcmp(e.english, english) == 0) return e.translated[static_cast<std::size_t>(lang) - 1];
    }
    return english;
}

std::string Texts::f(const char* english, std::initializer_list<std::string_view> args) const {
    const std::string_view pattern = (*this)(english);
    std::string out;
    out.reserve(pattern.size() + 32);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 2 < pattern.size() && pattern[i + 1] >= '0' && pattern[i + 1] <= '9' &&
            pattern[i + 2] == '}') {
            const std::size_t n = static_cast<std::size_t>(pattern[i + 1] - '0');
            if (n < args.size()) out += *(args.begin() + n);
            i += 2;
            continue;
        }
        out += pattern[i];
    }
    return out;
}

}  // namespace model_reload
