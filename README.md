<div dir="rtl">

# Floaty

<img src="res/icon-preview.png" alt="" width="320">

**תמונות שצפות מעל כל החלונות ב-Windows** — לוגו, חותמת או סימן מים. קובץ אחד, בלי התקנה.

&rlm;מבוסס על [Overlay Manager](https://github.com/tsoolgee/overlay-manager-win) של tsoolgee.

## מה יש

- שכבות בכל מסך: מיקום, גודל, אטימות, סיבוב וצבע
- מדבקות וטיימר עמדה מובנים
- קיצורי מקלדת, סטטיסטיקה ועדכון מתוך התוכנה

## התקנה

- מורידים את `Floaty.exe` מ-[Releases](../../releases) ומריצים.
- ל-Windows 10 בלי אינטרנט: `Floaty-offline.exe`.
- מגיעים מ-Overlay Manager? השכבות עוברות לבד.

## בנייה

</div>

```bash
nuget install Microsoft.Web.WebView2 -OutputDirectory packages -ExcludeVersion
cmake -S . -B build -A x64 -DWEBVIEW2_DIR="$PWD/packages/Microsoft.Web.WebView2"
cmake --build build --config Release
```

<div dir="rtl">

## רישיון

MIT, כמו המקור — ראו [LICENSE](LICENSE).

</div>
