# BookOS Sample Books

Five pre-formatted books you can drag-and-drop straight onto your SD card.

Each folder follows the BookOS spec exactly:

```
<book>/
  meta.txt              TITLE= AUTHOR= GENRE=
  0001.txt              one TXT file = one page
  0002.txt              ~80-100 words, manual line breaks
  ...
```

## Included

| Folder                       | Title                       | Author              | Pages |
|------------------------------|-----------------------------|---------------------|-------|
| `The_Last_Olympian_Demo/`    | The Last Olympian (Demo)    | Original demo       | 5     |
| `A_Scandal_In_Bohemia/`      | A Scandal in Bohemia        | Arthur Conan Doyle  | 6     |
| `The_Time_Machine/`          | The Time Machine            | H. G. Wells         | 6     |
| `Alice_In_Wonderland/`       | Alice in Wonderland         | Lewis Carroll       | 6     |
| `Frankenstein_Letter_1/`     | Frankenstein - Letter 1     | Mary Shelley        | 5     |

All non-demo entries are short, hand-formatted excerpts from
**public-domain** Project Gutenberg texts so you can ship BookOS with a
real library out of the box.

## How to install

Copy any (or all) of these folders into:

```
/WATCH/BookOS/Books/
```

on your SD card. Next boot, BookOS will pick them up automatically
during the **ARCHIVE RECOVERY** scan and you'll see them in the carousel.

To extend a book past the excerpt:

1. Paste the rest of the source text into a plain `.txt` file.
2. Hand-split into 80–100 word chunks with manual line breaks
   (28–38 chars per line, blank line between paragraphs).
3. Save each chunk as `0007.txt`, `0008.txt`, ... in the same folder.

The reader will pick up the new pages on next launch.
