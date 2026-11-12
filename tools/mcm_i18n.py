# MCM の画面（config.json）と、言語ごとの訳ファイル（Interface/Translations/MiniSkirt_<言語>.txt）を作る。
# 文言はこのファイルだけで直す。config.json には "$MSK_..." の鍵だけを書き、ゲームが言語に合わせて置き換える。
# 訳ファイルは UTF-16 LE（BOM 付き）・CRLF・「鍵<TAB>文」の形（Skyrim の決まり）。
# 使い方: python -X utf8 -I tools/mcm_i18n.py
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"

# 画面の並び（鍵の名前 → 種類など）。鍵は Text / Help で終え、ほかの鍵の頭と重ならないようにする
LAYOUT = [
    {"type": "header", "key": "General"},
    {"type": "toggle", "key": "Enabled", "id": "bEnabled:General"},
    {"type": "toggle", "key": "Elderly", "id": "bExcludeElderly:General"},
    {"type": "empty"},
    {"type": "header", "key": "Shape"},
    {"type": "slider", "key": "Hem", "id": "fSkirtLength:General", "min": 17, "max": 100, "step": 1, "fmt": "{0}"},
    {"type": "slider", "key": "HipEase", "id": "fHipEase:General", "min": 0, "max": 8, "step": 0.5, "fmt": "{1}"},
    {"type": "slider", "key": "Flare", "id": "fFlareAngle:General", "min": 0, "max": 30, "step": 1, "fmt": "{0}"},
    {"type": "header", "key": "Legs", "position": 1},
    {"type": "slider", "key": "LegTop", "id": "fBandTop:General", "min": 50, "max": 90, "step": 1, "fmt": "{0}"},
    {"type": "toggle", "key": "Underwear", "id": "bUnderwear:General"},
    {"type": "slider", "key": "Gap", "id": "fSkirtGap:General", "min": 0, "max": 8, "step": 0.5, "fmt": "{1}"},
    {"type": "slider", "key": "GapStiff", "id": "fSkirtGapNoBone:General", "min": 0, "max": 10, "step": 0.5, "fmt": "{1}"},
]

# 日本語のフォントに無い字（例: 裾、≒）は使わない。目安は「数: 説明」の形にする（≒ は空白に見えた）。
# スライダーの値の欄は大文字で表示される（cm → CM）ので、単位は項目名に書く
T = {}

T["JAPANESE"] = {
    "General": "全体",
    "Enabled": ("有効", "オンにすると、女性 NPC のスカート丈を変更します。また、服に付いている脚を隠して、使っている体（CBBE など）の脚を表示します。"),
    "Elderly": ("年配の女性を外す", "オンにすると、年配の女性（種族が年配の NPC）には何もしません。スカートは元の長さのままで、脚も足しません。"),
    "Shape": "スカートの形",
    "Hem": ("スカート丈（%）", "ウエストバンドからすそまでの長さ。バニラの標準的な長いドレスのすそ（床から約 8 cm）を 100 とします。17: マイクロミニ（最短）/ 27: 太ももの付け根の下 / 43: 太ももの真ん中 / 49: ひざの少し上 / 56: ひざ / 100: 元の長さ。100 では、床まで届く服でも、どの服も長さも形も元のままです。元の服より長くはならず、元のスカートがこの丈より短いときも元のままです。"),
    "HipEase": ("お尻まわりのゆとり", "スカートを、お尻・太もものいちばん外に出ている所からどれだけ離してふくらませるか。お尻がスカートを突き抜けるときは大きくします。0 だと体にぴったりになり、立っているときでも体が突き抜けやすくなります。"),
    "Flare": ("すその広がり角度", "お尻のふくらみより下で、すそを外へ広げる角度（度）。0 だと真下にまっすぐ、大きくするほどすそが広がります。"),
    "Legs": "スカートの中の脚",
    "LegTop": ("足す脚の上端の高さ", "スカートを短くすると、服の中の脚がとちゅうで切れて見えます。そこで服の中の脚を消し、代わりに裸の脚を足首からこの高さまで足します。80: 腰骨の少し下（股・お尻まで見える）、64: 股の少し下（股・お尻は見えない）。"),
    "Underwear": ("下着を着ける", "足した脚を、下着を着けた体にします。NPC の持ち物や服装から下着らしい物を選び、無ければ入っている下着 MOD から NPC ごとに 1 つ決めます（下着 MOD が無ければ裸のまま）。"),
    "Gap": ("スカートと脚のすき間", "スカートを脚から離す幅。近すぎる所だけ外へ押し出します（お尻の揺れに合わせてスカートも動く服）。お尻がスカートから突き抜けるときは大きくします。"),
    "GapStiff": ("すき間（スカートが揺れない服）", "お尻が揺れてもスカートがいっしょに動かない服での、スカートと脚のすき間。突き抜けやすいので広めにします。"),
}

T["ENGLISH"] = {
    "General": "General",
    "Enabled": ("Enabled", "When on, the skirt length of women NPCs is changed. The legs that come with the outfit are also hidden, and the legs of the body you use (CBBE, etc.) are shown instead."),
    "Elderly": ("Exclude elderly women", "When on, elderly women (NPCs of the elder race) are left alone: their skirts keep the original length and no legs are added."),
    "Shape": "Skirt Shape",
    "Hem": ("Skirt length (%)", "Length from the waistband to the hem, where 100 is the hem of a standard long vanilla dress (about 8 cm above the floor). 17: micro mini (shortest) / 27: just below the top of the thigh / 43: mid-thigh / 49: just above the knee / 56: knee / 100: original length. At 100, every outfit keeps its original length and shape, even one that reaches the floor. Skirts never get longer than the original outfit, and a skirt already shorter than this is left as it is."),
    "HipEase": ("Room around the hips", "How far the skirt stands off from the widest part of the hips and thighs. Increase it if the buttocks poke through the skirt. At 0 the skirt fits skin-tight and the body pokes through more easily, even when standing."),
    "Flare": ("Hem flare angle", "Angle (degrees) at which the skirt spreads outward below the hips. 0 hangs straight down; larger values flare the hem out more."),
    "Legs": "Legs Under the Skirt",
    "LegTop": ("Top of the added legs", "A shortened skirt would show the outfit's legs cut off partway. So the outfit's legs are hidden and bare legs are added from the ankles up to this height. 80: just below the hip bones (crotch and buttocks visible), 64: just below the crotch (not visible)."),
    "Underwear": ("Wear underwear", "Puts underwear on the added legs. Picks something underwear-like from the NPC's inventory or outfit; otherwise picks one per NPC from installed underwear mods (bare if there are none)."),
    "Gap": ("Gap between skirt and legs", "How far the skirt is kept from the legs. Only the parts that are too close are pushed out (outfits whose skirt moves with the buttocks). Increase it if the buttocks poke through."),
    "GapStiff": ("Gap (skirts that don't sway)", "Gap between skirt and legs for outfits whose skirt does not move when the buttocks jiggle. Set it wider, since these poke through more easily."),
}

T["FRENCH"] = {
    "General": "Général",
    "Enabled": ("Activé", "Activé : la longueur des jupes des PNJ féminins est modifiée. De plus, les jambes fournies avec la tenue sont masquées et celles du corps que vous utilisez (CBBE, etc.) sont affichées."),
    "Elderly": ("Exclure les femmes âgées", "Activé : les femmes âgées (PNJ de la race des anciens) ne sont pas modifiées. Leur jupe garde sa longueur d'origine et aucune jambe n'est ajoutée."),
    "Shape": "Forme de la jupe",
    "Hem": ("Longueur de la jupe (%)", "Longueur de la ceinture à l'ourlet, 100 étant l'ourlet d'une robe longue standard du jeu (environ 8 cm au-dessus du sol). 17: micro-mini (la plus courte) / 27: juste sous le haut de la cuisse / 43: mi-cuisse / 49: juste au-dessus du genou / 56: genou / 100: longueur d'origine. À 100, chaque tenue garde sa longueur et sa forme d'origine, même si elle touche le sol. Une jupe n'est jamais rallongée, et une jupe déjà plus courte reste telle quelle."),
    "HipEase": ("Aisance aux hanches", "Distance entre la jupe et la partie la plus large des hanches et des cuisses. Augmentez si les fesses traversent la jupe. À 0, la jupe colle au corps et le corps la traverse plus facilement, même debout."),
    "Flare": ("Évasement de l'ourlet", "Angle (degrés) d'évasement de la jupe sous les hanches. 0 tombe droit ; plus la valeur est grande, plus l'ourlet s'évase."),
    "Legs": "Jambes sous la jupe",
    "LegTop": ("Haut des jambes ajoutées", "Une jupe raccourcie montrerait les jambes de la tenue coupées à mi-hauteur. Elles sont donc masquées et des jambes nues sont ajoutées des chevilles jusqu'à cette hauteur. 80: juste sous les hanches (entrejambe et fesses visibles), 64: juste sous l'entrejambe (non visibles)."),
    "Underwear": ("Porter des sous-vêtements", "Ajoute des sous-vêtements aux jambes ajoutées. Choisit un sous-vêtement dans l'inventaire ou la tenue du PNJ ; sinon un par PNJ parmi les mods de sous-vêtements installés (nu s'il n'y en a pas)."),
    "Gap": ("Écart jupe / jambes", "Distance gardée entre la jupe et les jambes. Seules les parties trop proches sont repoussées (tenues dont la jupe suit le mouvement des fesses). Augmentez si les fesses traversent."),
    "GapStiff": ("Écart (jupes rigides)", "Écart pour les tenues dont la jupe ne suit pas le mouvement des fesses. Mettez-le plus grand, car elles traversent plus facilement."),
}

T["GERMAN"] = {
    "General": "Allgemein",
    "Enabled": ("Aktiviert", "Wenn an: Die Rocklänge weiblicher NPCs wird geändert. Außerdem werden die Beine der Kleidung ausgeblendet und die Beine des verwendeten Körpers (CBBE usw.) angezeigt."),
    "Elderly": ("Ältere Frauen ausnehmen", "Wenn an: Ältere Frauen (NPCs der Rasse der Alten) bleiben unverändert. Ihr Rock behält die ursprüngliche Länge, und es werden keine Beine hinzugefügt."),
    "Shape": "Rockform",
    "Hem": ("Rocklänge (%)", "Länge vom Bund bis zum Saum, wobei 100 der Saum eines normalen langen Kleides aus dem Spiel ist (etwa 8 cm über dem Boden). 17: Mikro-Mini (am kürzesten) / 27: knapp unter dem Oberschenkelansatz / 43: Mitte des Oberschenkels / 49: knapp über dem Knie / 56: Knie / 100: ursprüngliche Länge. Bei 100 behält jede Kleidung Länge und Form, auch wenn sie bis zum Boden reicht. Röcke werden nie länger, und ein Rock, der schon kürzer ist, bleibt wie er ist."),
    "HipEase": ("Weite an der Hüfte", "Abstand des Rocks zur breitesten Stelle von Hüfte und Oberschenkeln. Erhöhen, wenn der Po durch den Rock ragt. Bei 0 liegt der Rock hauteng an, und der Körper ragt leichter durch, selbst im Stehen."),
    "Flare": ("Ausstellwinkel des Saums", "Winkel (Grad), in dem der Rock unterhalb der Hüfte nach außen geht. 0 fällt gerade nach unten; größere Werte stellen den Saum weiter aus."),
    "Legs": "Beine unter dem Rock",
    "LegTop": ("Obere Grenze der Beine", "Ein gekürzter Rock würde die Beine der Kleidung abgeschnitten zeigen. Deshalb werden sie ausgeblendet und nackte Beine von den Knöcheln bis zu dieser Höhe hinzugefügt. 80: knapp unter den Hüftknochen (Schritt und Po sichtbar), 64: knapp unter dem Schritt (nicht sichtbar)."),
    "Underwear": ("Unterwäsche tragen", "Gibt den hinzugefügten Beinen Unterwäsche. Wählt etwas Passendes aus Inventar oder Kleidung des NPCs, sonst pro NPC eines aus installierten Unterwäsche-Mods (nackt, wenn keine vorhanden)."),
    "Gap": ("Abstand Rock / Beine", "Abstand, den der Rock zu den Beinen hält. Nur zu nahe Stellen werden nach außen gedrückt (Kleidung, deren Rock sich mit dem Po bewegt). Erhöhen, wenn der Po durchragt."),
    "GapStiff": ("Abstand (starre Röcke)", "Abstand für Kleidung, deren Rock sich nicht mit dem Po mitbewegt. Größer wählen, da sie leichter durchragt."),
}

T["SPANISH"] = {
    "General": "General",
    "Enabled": ("Activado", "Si está activado, cambia el largo de la falda de las PNJ. Además, oculta las piernas que trae la ropa y muestra las del cuerpo que usas (CBBE, etc.)."),
    "Elderly": ("Excluir a las ancianas", "Si se activa, las ancianas (PNJ de la raza de los ancianos) no se modifican: su falda conserva el largo original y no se añaden piernas."),
    "Shape": "Forma de la falda",
    "Hem": ("Largo de la falda (%)", "Largo desde la cinturilla hasta el bajo, siendo 100 el bajo de un vestido largo normal del juego (unos 8 cm sobre el suelo). 17: micro mini (la más corta) / 27: justo bajo el inicio del muslo / 43: mitad del muslo / 49: justo sobre la rodilla / 56: rodilla / 100: largo original. Con 100, toda la ropa conserva su largo y forma originales, aunque llegue al suelo. Las faldas nunca quedan más largas, y una falda ya más corta se deja como está."),
    "HipEase": ("Holgura en las caderas", "Distancia entre la falda y la parte más ancha de caderas y muslos. Auméntala si los glúteos atraviesan la falda. Con 0 la falda queda pegada al cuerpo y este la atraviesa con más facilidad, incluso de pie."),
    "Flare": ("Ángulo de vuelo del bajo", "Ángulo (grados) con que la falda se abre bajo las caderas. 0 cae recta; valores mayores abren más el bajo."),
    "Legs": "Piernas bajo la falda",
    "LegTop": ("Límite superior de las piernas", "Una falda acortada mostraría las piernas de la ropa cortadas a medias. Por eso se ocultan y se añaden piernas desnudas desde los tobillos hasta esta altura. 80: justo bajo las caderas (entrepierna y glúteos visibles), 64: justo bajo la entrepierna (no visibles)."),
    "Underwear": ("Llevar ropa interior", "Pone ropa interior en las piernas añadidas. Elige algo del inventario o atuendo del PNJ; si no hay, una por PNJ de los mods de ropa interior instalados (desnudo si no hay ninguno)."),
    "Gap": ("Separación falda / piernas", "Distancia que la falda guarda respecto a las piernas. Solo se empujan hacia fuera las partes demasiado cercanas (ropa cuya falda se mueve con los glúteos). Auméntala si los glúteos la atraviesan."),
    "GapStiff": ("Separación (faldas rígidas)", "Separación para ropa cuya falda no se mueve con los glúteos. Ponla más amplia, ya que se atraviesa con más facilidad."),
}

T["ITALIAN"] = {
    "General": "Generale",
    "Enabled": ("Attivo", "Se attivo, cambia la lunghezza della gonna dei PNG femminili. Inoltre nasconde le gambe incluse nel vestito e mostra quelle del corpo che usi (CBBE, ecc.)."),
    "Elderly": ("Escludi le donne anziane", "Se attivo, le donne anziane (PNG della razza degli anziani) non vengono modificate: la gonna mantiene la lunghezza originale e non si aggiungono gambe."),
    "Shape": "Forma della gonna",
    "Hem": ("Lunghezza della gonna (%)", "Lunghezza dalla cintura all'orlo, dove 100 è l'orlo di un normale vestito lungo del gioco (circa 8 cm dal pavimento). 17: micro mini (la più corta) / 27: appena sotto l'attaccatura della coscia / 43: metà coscia / 49: appena sopra il ginocchio / 56: ginocchio / 100: lunghezza originale. A 100 ogni vestito mantiene lunghezza e forma originali, anche se arriva al pavimento. Le gonne non diventano mai più lunghe, e una gonna già più corta resta com'è."),
    "HipEase": ("Agio sui fianchi", "Distanza della gonna dal punto più largo di fianchi e cosce. Aumentala se i glutei attraversano la gonna. A 0 la gonna aderisce al corpo e il corpo la attraversa più facilmente, anche da in piedi."),
    "Flare": ("Svasatura dell'orlo", "Angolo (gradi) con cui la gonna si allarga sotto i fianchi. 0 scende dritta; valori più alti allargano di più l'orlo."),
    "Legs": "Gambe sotto la gonna",
    "LegTop": ("Limite superiore delle gambe", "Una gonna accorciata mostrerebbe le gambe del vestito tagliate a metà. Perciò vengono nascoste e si aggiungono gambe nude dalle caviglie fino a questa altezza. 80: appena sotto le anche (inguine e glutei visibili), 64: appena sotto l'inguine (non visibili)."),
    "Underwear": ("Indossa biancheria", "Aggiunge biancheria intima alle gambe aggiunte. Sceglie qualcosa dall'inventario o dal vestito del PNG; altrimenti una per PNG dalle mod di biancheria installate (nuda se non ce ne sono)."),
    "Gap": ("Distanza gonna / gambe", "Distanza che la gonna mantiene dalle gambe. Solo le parti troppo vicine vengono spinte fuori (vestiti la cui gonna si muove con i glutei). Aumentala se i glutei la attraversano."),
    "GapStiff": ("Distanza (gonne rigide)", "Distanza per i vestiti la cui gonna non si muove con i glutei. Impostala più ampia, perché si attraversa più facilmente."),
}

T["POLISH"] = {
    "General": "Ogólne",
    "Enabled": ("Włączone", "Po włączeniu zmienia się długość spódnic postaci kobiecych. Ponadto nogi dołączone do stroju są ukrywane, a wyświetlane są nogi używanego ciała (CBBE itp.)."),
    "Elderly": ("Pomiń starsze kobiety", "Po włączeniu starsze kobiety (NPC rasy starców) nie są zmieniane: spódnica zachowuje oryginalną długość i nie są dodawane nogi."),
    "Shape": "Kształt spódnicy",
    "Hem": ("Długość spódnicy (%)", "Długość od paska do brzegu, gdzie 100 to brzeg zwykłej długiej sukni z gry (około 8 cm nad podłogą). 17: mikro mini (najkrótsza) / 27: tuż pod nasadą uda / 43: połowa uda / 49: tuż nad kolanem / 56: kolano / 100: oryginalna długość. Przy 100 każdy strój zachowuje oryginalną długość i kształt, nawet sięgający podłogi. Spódnica nigdy nie będzie dłuższa, a spódnica już krótsza zostaje bez zmian."),
    "HipEase": ("Luz na biodrach", "Jak daleko spódnica odstaje od najszerszego miejsca bioder i ud. Zwiększ, jeśli pośladki przebijają spódnicę. Przy 0 spódnica przylega do ciała i ciało łatwiej ją przebija, nawet na stojąco."),
    "Flare": ("Kąt rozkloszowania", "Kąt (stopnie), pod jakim spódnica rozchodzi się poniżej bioder. 0 opada prosto; większe wartości bardziej rozszerzają brzeg."),
    "Legs": "Nogi pod spódnicą",
    "LegTop": ("Górna granica nóg", "Skrócona spódnica pokazałaby ucięte w połowie nogi stroju. Dlatego są ukrywane, a nagie nogi dodawane od kostek do tej wysokości. 80: tuż pod kośćmi biodrowymi (krocze i pośladki widoczne), 64: tuż pod kroczem (niewidoczne)."),
    "Underwear": ("Noś bieliznę", "Dodaje bieliznę do dodanych nóg. Wybiera coś z ekwipunku lub stroju NPC; w przeciwnym razie jedną na NPC z zainstalowanych modów z bielizną (nago, jeśli ich brak)."),
    "Gap": ("Odstęp spódnica / nogi", "Odstęp spódnicy od nóg. Na zewnątrz odsuwane są tylko zbyt bliskie miejsca (stroje, których spódnica porusza się z pośladkami). Zwiększ, jeśli pośladki przebijają."),
    "GapStiff": ("Odstęp (sztywne spódnice)", "Odstęp dla strojów, których spódnica nie porusza się z pośladkami. Ustaw większy, bo łatwiej je przebić."),
}

T["RUSSIAN"] = {
    "General": "Общие",
    "Enabled": ("Включено", "Если включено, длина юбок женских NPC меняется. Кроме того, ноги, входящие в одежду, скрываются, и показываются ноги используемого тела (CBBE и т. п.)."),
    "Elderly": ("Не трогать пожилых женщин", "Если включить, пожилые женщины (NPC расы стариков) не изменяются: юбка сохраняет исходную длину, ноги не добавляются."),
    "Shape": "Форма юбки",
    "Hem": ("Длина юбки (%)", "Длина от пояса до подола, где 100 — подол обычного длинного платья из игры (около 8 см от пола). 17: микро-мини (самая короткая) / 27: чуть ниже начала бедра / 43: середина бедра / 49: чуть выше колена / 56: колено / 100: исходная длина. При 100 любая одежда сохраняет исходную длину и форму, даже доходящая до пола. Юбка никогда не становится длиннее, а юбка, которая уже короче, остаётся как есть."),
    "HipEase": ("Свобода на бёдрах", "Насколько юбка отстоит от самой широкой части бёдер. Увеличьте, если ягодицы проходят сквозь юбку. При 0 юбка прилегает вплотную, и тело легче проходит сквозь неё, даже стоя."),
    "Flare": ("Угол расклёшенности", "Угол (градусы), под которым юбка расходится ниже бёдер. 0 — прямо вниз; чем больше, тем шире подол."),
    "Legs": "Ноги под юбкой",
    "LegTop": ("Верхняя граница ног", "Укороченная юбка показала бы ноги одежды, обрезанные посередине. Поэтому они скрываются, а голые ноги добавляются от щиколоток до этой высоты. 80: чуть ниже тазовых костей (пах и ягодицы видны), 64: чуть ниже паха (не видны)."),
    "Underwear": ("Носить бельё", "Добавляет бельё на добавленные ноги. Выбирает подходящее из инвентаря или одежды NPC; иначе — по одному на NPC из установленных модов белья (голые, если их нет)."),
    "Gap": ("Зазор юбка / ноги", "Насколько юбка отстоит от ног. Наружу сдвигаются только слишком близкие места (одежда, юбка которой движется вместе с ягодицами). Увеличьте, если ягодицы проходят насквозь."),
    "GapStiff": ("Зазор (жёсткие юбки)", "Зазор для одежды, юбка которой не движется вместе с ягодицами. Ставьте больше — сквозь них проходит легче."),
}

T["CZECH"] = {
    "General": "Obecné",
    "Enabled": ("Zapnuto", "Po zapnutí se změní délka sukní ženských NPC. Navíc se skryjí nohy, které patří k oblečení, a zobrazí se nohy používaného těla (CBBE apod.)."),
    "Elderly": ("Vynechat starší ženy", "Po zapnutí se starší ženy (NPC rasy starců) nemění: sukně si ponechá původní délku a nohy se nepřidávají."),
    "Shape": "Tvar sukně",
    "Hem": ("Délka sukně (%)", "Délka od pasu po lem, kde 100 je lem běžných dlouhých šatů ze hry (asi 8 cm nad podlahou). 17: mikro mini (nejkratší) / 27: těsně pod začátkem stehna / 43: polovina stehna / 49: těsně nad kolenem / 56: koleno / 100: původní délka. Při 100 si každé oblečení ponechá původní délku i tvar, i když sahá až na zem. Sukně nikdy nebude delší a sukně, která je už kratší, zůstane, jak je."),
    "HipEase": ("Volnost v bocích", "Jak daleko sukně odstává od nejširšího místa boků a stehen. Zvyšte, pokud hýždě prochází sukní. Při 0 sukně těsně přiléhá a tělo jí snáze prochází, i ve stoje."),
    "Flare": ("Úhel rozšíření lemu", "Úhel (stupně), pod kterým se sukně pod boky rozšiřuje. 0 padá rovně dolů; větší hodnoty lem více rozšíří."),
    "Legs": "Nohy pod sukní",
    "LegTop": ("Horní hranice nohou", "Zkrácená sukně by ukázala nohy oblečení useknuté v půli. Proto se skryjí a přidají se nahé nohy od kotníků do této výšky. 80: těsně pod kyčelními kostmi (rozkrok a hýždě vidět), 64: těsně pod rozkrokem (nevidět)."),
    "Underwear": ("Nosit spodní prádlo", "Přidá spodní prádlo na přidané nohy. Vybere něco z inventáře nebo oblečení NPC; jinak jedno na NPC z nainstalovaných modů prádla (nahé, pokud žádné nejsou)."),
    "Gap": ("Mezera sukně / nohy", "Jak daleko sukně zůstává od nohou. Ven se odsunou jen příliš blízká místa (oblečení, jehož sukně se hýbe s hýžděmi). Zvyšte, pokud hýždě prochází."),
    "GapStiff": ("Mezera (tuhé sukně)", "Mezera pro oblečení, jehož sukně se nehýbe s hýžděmi. Nastavte větší, protože se snáze prochází."),
}

T["CHINESE"] = {  # 簡体字（中国語の枠はゲームに 1 つだけ。利用者の多い簡体字にした）
    "General": "常规",
    "Enabled": ("启用", "启用后，会改变女性 NPC 的裙长。另外，会隐藏衣服自带的双腿，显示你使用的身体（CBBE 等）的双腿。"),
    "Elderly": ("排除年长女性", "开启后，不改动年长女性（老年种族的 NPC）：裙子保持原来的长度，也不添加双腿。"),
    "Shape": "裙子形状",
    "Hem": ("裙长（%）", "从腰头到裙摆的长度，以游戏中普通长裙的裙摆（离地约 8 cm）为 100。17: 超短裙（最短）/ 27: 大腿根部稍下 / 43: 大腿中部 / 49: 膝盖稍上 / 56: 膝盖 / 100: 原来的长度。设为 100 时，所有衣服（即使拖到地面）都保持原来的长度和形状。裙子不会变得更长，原本就更短的裙子保持不变。"),
    "HipEase": ("臀部宽松度", "裙子与臀部和大腿最宽处的距离。臀部穿出裙子时请调大。设为 0 时裙子紧贴身体，即使站着身体也更容易穿出。"),
    "Flare": ("裙摆张开角度", "臀部以下裙子向外张开的角度（度）。0 为垂直向下，数值越大裙摆越张开。"),
    "Legs": "裙下的双腿",
    "LegTop": ("添加的双腿上缘", "裙子缩短后，衣服里的双腿会在中途被截断而露出来。因此会隐藏衣服的双腿，改为从脚踝到这个高度添加裸露的双腿。80: 髋骨稍下（能看到胯部和臀部），64: 胯部稍下（看不到）。"),
    "Underwear": ("穿上内衣", "让添加的双腿穿上内衣。会从 NPC 的物品或服装中挑选类似内衣的物品；如果没有，则从已安装的内衣 MOD 中为每个 NPC 选一件（没有内衣 MOD 则保持裸露）。"),
    "Gap": ("裙子与双腿的间隙", "裙子与双腿保持的距离。只把太靠近的部分向外推（裙子会随臀部摆动的服装）。臀部穿出时请调大。"),
    "GapStiff": ("间隙（不会摆动的裙子）", "臀部摆动时裙子不会跟着动的服装所用的间隙。更容易穿出，请设大一些。"),
}


def key(name, part):
    return f"$MSK_{name}{part}"


def build_config():
    content = []
    for item in LAYOUT:
        t = item["type"]
        if t == "empty":
            content.append({"type": "empty"})
            continue
        if t == "header":
            h = {"text": key(item["key"], "Text"), "type": "header"}
            if "position" in item:
                h["position"] = item["position"]
            content.append(h)
            continue
        e = {"id": item["id"], "text": key(item["key"], "Text"), "help": key(item["key"], "Help"), "type": t}
        if t == "toggle":
            e["valueOptions"] = {"sourceType": "ModSettingBool"}
        else:
            e["valueOptions"] = {"min": item["min"], "max": item["max"], "step": item["step"], "formatString": item["fmt"], "sourceType": "ModSettingFloat"}
        content.append(e)
    cfg = {"modName": "MiniSkirt", "displayName": "MiniSkirt", "minMcmVersion": 9, "cursorFillMode": "topToBottom", "content": content}
    p = ASSETS / "MCM/Config/MiniSkirt/config.json"
    p.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")


def build_translations():
    want = set()
    for item in LAYOUT:
        if item["type"] == "header":
            want.add(item["key"])
        elif item["type"] != "empty":
            want.add(item["key"])
    out = ASSETS / "Interface/Translations"
    out.mkdir(parents=True, exist_ok=True)
    for lang, d in T.items():
        assert set(d) == want, (lang, set(d) ^ want)
        lines = []
        for item in LAYOUT:
            if item["type"] == "empty":
                continue
            v = d[item["key"]]
            if isinstance(v, str):
                lines.append(f"{key(item['key'], 'Text')}\t{v}")
            else:
                lines.append(f"{key(item['key'], 'Text')}\t{v[0]}")
                lines.append(f"{key(item['key'], 'Help')}\t{v[1]}")
        for s in lines:
            assert "\n" not in s and s.count("\t") == 1, s
        data = "\r\n".join(lines) + "\r\n"
        (out / f"MiniSkirt_{lang}.txt").write_bytes(b"\xff\xfe" + data.encode("utf-16-le"))


build_config()
build_translations()
print("ok", len(T), "languages")
