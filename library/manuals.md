# Official manuals for the "Manuals" book

Public-domain US government publications (and a few freely licensed ones)
worth carrying. Download the PDFs into one folder, then:

```bash
pip3 install pymupdf
python3 library/docs2doba.py ~/Doba/manuals /Volumes/SSD/doba/lib/manuals \
    --name "Manuals" --source "US Army / FEMA / CDC · public domain"
tools/prepare-sd-mac.sh copy-lib /Volumes/SSD/doba/lib
```

PDFs with bookmarks are split per chapter, and each chapter is found by its
own name ("shelter", "water"), not only by the manual's.

Where to get them: the Army Publishing Directorate (armypubs.army.mil — the
"public release" ones), fema.gov, ready.gov, cdc.gov, usda.gov. Search the
number; older editions are on archive.org and other mirrors.

## Survival and fieldcraft
- ATP 3-50.21 *Survival* (successor to FM 21-76 / FM 3-05.70)
- FM 21-76-1 *Survival, Evasion, and Recovery* (pocket guide)
- FM 3-25.26 / TC 3-25.26 *Map Reading and Land Navigation*
- FM 3-97.61 *Military Mountaineering*
- FM 31-70 / FM 31-71 *Basic Cold Weather Manual* / *Northern Operations*
- TC 3-97.61 *Military Mountaineering* (current)

## Medicine and hygiene
- TC 4-02.1 *First Aid* (successor to FM 4-25.11)
- FM 21-10 / ATP 4-25.12 *Field Hygiene and Sanitation*
- ST 31-91B *Special Forces Medical Handbook* (older, widely mirrored)
- TC 4-02.3 *Field Hygiene and Sanitation* (current)
- CDC: *Emergency Preparedness and Response* sheets (water, food, mold, heat, cold)

## Water, food, shelter, power
- FEMA *Are You Ready? An In-depth Guide to Citizen Preparedness*
- FEMA *Planning Guidance for Response to a Nuclear Detonation*
- USDA *Complete Guide to Home Canning*
- TM 5-813-series *Water Supply* (sources, treatment)
- FM 5-125 *Rigging Techniques, Procedures, and Applications*
- FM 5-434 *Earthmoving Operations*; FM 5-426 *Carpentry*

## Radio and comms
- FM 24-18 *Tactical Single-Channel Radio Communications Techniques*
- TC 9-64 *Communications-Electronics Fundamentals*

## Not public domain, free to download for personal use
- Hesperian *Where There Is No Doctor* / *Where There Is No Dentist*
  (licence allows personal and non-commercial use; check before sharing)

Your own files (notes, scans of papers you'll need, recipes, frequency lists)
go the same way into a second book:

```bash
python3 library/docs2doba.py ~/Doba/my-files /Volumes/SSD/doba/lib/mine --name "My files"
```

PDF, EPUB, HTML, Markdown and plain text are read; scans without a text layer
come out empty (OCR them first, e.g. `ocrmypdf in.pdf out.pdf`).
