ცნობა · სანდრო → ლუკა, თემო, დემეტრე და მათი Claude · 17.10.2026

# აპის სახელია **Sarke** (სარკე): რა გაკეთდა და რა რჩება თქვენზე

Claude, ეს ფაილი ყველა სესიისთვისაა. სანდრომ აპს დაარქვა **Sarke** (ქართულად **სარკე**) და აირჩია ახალი ლოგო. **ლუკა, გთხოვ, დაადასტურე გუნდისთვის** — სახელი ყველას ეხება.

## რა გაკეთდა (ბრენჩი `sandro/sarke-brand`, `sandro/viewer-design`-ის თავზე)

- **სახელი viewer-ში, tray-ში და შეტყობინებებში:** „Sarke“ (ფანჯარა, taskbar, tray), „SARKE“ (label-ები), „sarke“ (სათაურის ზოლი, როცა სტრიმი არ არის). კოდში ერთ ადგილასაა: `kAppName` / `kAppNameIdle` tokens-ში.
- **ლოგო: „ს caret“** — ქართული **ს** პიქსელებით + accent-ის ბლოკ-კურსორი (ისეთივე, როგორიც viewer-ის ეკრანებზე ციმციმებს). სპეციფიკაცია: `docs/DESIGN.md`, ნაწილი 9. ფერები და ფორმა იგივე დარჩა (ნარინჯისფერი accent).
- **`docs/brand/sarke.ico`** — ხატულა exe-სთვის, ზომები 16–256 px (Win32 `LoadImage`-ით შემოწმებულია).
- **`tools/ui_preview_logo.exe`** — ლოგოს ყველა ზომა ერთ სურათზე (`--ico <file>` ქმნის `.ico`-ს).
- viewer-ის LINKED ეკრანზე მინიშნება ახლა შენს გადაწყვეტილებას ემთხვევა: „on the other pc press ctrl+alt+shift+w to send a window“.

**არ შეცვლილა:** კოდის სახელები (`namespace kvm`, `kvm_common`, ფანჯრის კლასები) — მომხმარებელი მათ ვერ ხედავს, და შეცვლა ყველას ფაილს შეეხებოდა.

## ლუკა (შენი ფაილები)

1. **exe-ების სახელები** (`kvm_host.exe` / `kvm_client.exe`) — შენი გადასაწყვეტია (მაგ. `sarke_host.exe` / `sarke_client.exe`, ან ერთი `sarke.exe`, რადგან v2 სიმეტრიულია). `tools/test_package/*.cmd` და firewall-ის წესი მათ მიხედვით.
2. **`.rc` ფაილი** ორივე exe-ში: Windows-ის შეტყობინებაში აპის სახელად ახლა exe-ს ფაილის სახელი ჩანს; `FileDescription`-ით იქნება „Sarke“. ხატულა Explorer-ში და pinned shortcut-ზე `sarke.ico`-დან:
   ```rc
   #include <winres.h>
   1 ICON "../docs/brand/sarke.ico"          // გზა .rc ფაილიდან
   VS_VERSION_INFO VERSIONINFO
    FILEVERSION 0,2,0,0
    PRODUCTVERSION 0,2,0,0
   BEGIN
     BLOCK "StringFileInfo"
     BEGIN
       BLOCK "040904b0"
       BEGIN
         VALUE "FileDescription", "Sarke"
         VALUE "ProductName", "Sarke"
       END
     END
     BLOCK "VarFileInfo"
     BEGIN
       VALUE "Translation", 0x409, 1200
     END
   END
   ```
   CMake: `.rc` დაამატე ორივე `add_executable`-ს (MSVC მას თავად აწყობს).
3. **CLAUDE.md:** სათაური და აღწერა „KVM + Window Streaming“ → Sarke.
4. ფონტი (`assets/fonts/`) და tokens/draw helpers (`common/include/kvm/ui/`) — როგორც ადრე ვთხოვე (`ARTEFACT/სანდრო — პასუხი და viewer-ის ჩართვა`).

## თემო

`docs/DESIGN.md`, ნაწილი 3 (picker): footer-ში წერია „KVM HOST“. შეთავაზება: **„SARKE“** (v2-ში ორივე მხარე ერთნაირია). შენი ზედაპირია — შენ გადაწყვიტე.

## დემეტრე

edge glow-ს ტექსტი არ აქვს — შენთან არაფერი იცვლება.
