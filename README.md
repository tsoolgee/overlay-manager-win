<div dir="rtl">

# Floaty

<img src="res/icon-preview.png" alt="" width="320">

**תמונות שצפות מעל כל החלונות ב-Windows** — לוגו, חותמת או סימן מים. קובץ אחד, בלי התקנה.

&rlm;מבוסס על [Overlay Manager](https://github.com/tsoolgee/overlay-manager-win) של tsoolgee.

## מה יש

- שכבות בלי הגבלה, בכל מסך, עם גודל, מיקום, אטימות, סיבוב וצבע
- מיקום בגרירה על המסך, עם שמירה או ביטול
- לחיצות עוברות דרך השכבה
- `Ctrl+Alt+H` מסתיר ומציג הכול
- שמירה אוטומטית ועדכון מתוך התוכנה

## התקנה

מורידים את `Floaty.exe` מ-[Releases](../../releases) ומריצים.
צריך את רכיב WebView2 של מיקרוסופט (קיים ב-Windows 11 וברוב Windows 10).
מגיעים מ-Overlay Manager? השכבות עוברות לבד בהפעלה הראשונה.

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
