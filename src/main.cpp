#include <mast1c0re.hpp>
#include <common/list.hpp>
#include <ps/sce/usb/usb/usb.hpp>
#include <ps/sce/usb/filesystems/exfat/file.hpp>
#include <ps/sce/usb/filesystems/exfat/directory.hpp>
#include "elf/elf.hpp"

#define COPY_BAR_UPDATE_FREQUENCY 100
#define ELF_BUFFER  0x1000000

typedef void fEntry();
bool loadAndExecuteELF();
bool getELFToLoad(List<Usb> usbs, Usb* usb, exFAT::File* file);
bool copyUsbFileToBuffer(Usb* usb, uint8_t* buffer, exFAT::File file);
void setProgress(PS::Sce::MsgDialogProgressBar dialog, size_t downloaded, size_t total);

void main()
{
    // PS2 Breakout
    PS::Breakout::init();

    // Connect to debug server
    // PS::Debug.connect(IP(192, 168, 0, 7), 9017);

    // Keep executing PS2 ELF files
    while (loadAndExecuteELF());

    // Disconnect from debug server
    PS::Debug.disconnect();

    // Restore corruption
    PS::Breakout::restore();
}

bool loadAndExecuteELF()
{
    // ELF entry point
    fEntry* entry = nullptr;

    // Get a list of connected USBs
    List<Usb> usbs = Usb::list();

    // Get ELF to load
    Usb usb;
    exFAT::File file;

    if (getELFToLoad(usbs, &usb, &file) && copyUsbFileToBuffer(&usb, (uint8_t*)ELF_BUFFER, file))
    {
        // Parse ELF
        Elf* elf = (Elf*)ELF_BUFFER;

        // Validate header
        const char* error = elf->header.validate();
        if (error == nullptr)
        {
            // Load program sections into memory
            PS::Debug.printf("Enumerating program table...\n");
            ElfProgram* programs = elf->getProgramTable();
            for (uint32_t i = 0; i < elf->header.e_phnum; i++)
            {
                // Ignore non-loadable program sections
                if (programs[i].p_type != PT_LOAD)
                    continue;

                uint8_t* section = (uint8_t*)((uint32_t)elf + programs[i].p_offset);

                // Copy section from ELF section to given destination address
                PS::Debug.printf("Loading 0x%06x to 0x%06x (0x%06x)\n",
                    section,
                    programs[i].p_vaddr,
                    programs[i].p_filesz
                );
                PS2::memcpy(programs[i].p_vaddr, section, programs[i].p_filesz);
                if (programs[i].p_memsz > programs[i].p_filesz)
                    PS2::memset((void*)((uint32_t)programs[i].p_vaddr + programs[i].p_filesz), 0, programs[i].p_memsz - programs[i].p_filesz);
            }

            // Set entry point
            PS::Debug.printf("Entry: 0x%08x\n", elf->header.e_entry);
            entry = (fEntry*)elf->header.e_entry;
        }
        else
        {
            PS::Debug.printf("Header validation error: %s\n", error);
            PS::notification("Error: %s", error);
        }
    }

    // Cleanup
    for (uint32_t i = 0; i < usbs.size(); i++)
        usbs[i].unmount();
    usbs.free();

    // Execute loaded ELF
    if (entry != nullptr)
    {
        entry();
        return true;
    }

    return false;
}

bool getELFToLoad(List<Usb> usbs, Usb* usb, exFAT::File* file)
{
    if (usbs.size() == 0)
    {
        PS::notification("Error: No USB found");
        return false;
    }

    // Check each USB
    bool elfsFound = false;
    for (uint32_t i = 0; i < usbs.size(); i++)
    {
        // Open USB
        if (!usbs[i].open())
        {
            PS::Debug.printf("Error: Failed to open USB #%i\n", i + 1);
            continue;
        }

        // Mount USB
        if (!usbs[i].mount())
        {
            PS::Debug.printf("Error: Failed to mount USB #%i\n", i + 1);
            continue;
        }

        // Check "ELFs" directory exists
        PS::Debug.printf("Checking ELFs...\n");
        if (!usbs[i].exists("/ELFs/"))
        {
            PS::Debug.printf("Error: Directory \"/ELFs/\" does not exist on USB #%i\n", i + 1);
            continue;
        }

        // Loop each ELF in ELFs directory
        List<exFAT::File> files = usbs[i].files("/ELFs");
        PS::Debug.printf("Found %i files in ELFs directory\n", files.size());

        for (uint32_t j = 0; j < files.size(); j++)
        {
            // Ignore non ELF files
            if (!files[j].hasExtension("elf"))
                continue;

            elfsFound = true;
            PS::Debug.printf("Found ELF: %s\n", files[j].getName());

            char message[512];
            PS2::sprintf(message, "Do you want to load %s?", files[j].getName());
            if (PS::Sce::MsgDialogUserMessage::show(message, PS::Sce::MsgDialog::ButtonType::YESNO))
            {
                *usb = usbs[i];
                *file = files[j];
                return true;
            }
        }
    }

    PS::notification("Error: No ELF %s\n", elfsFound ? "selected" : "found");
    return false;
}

bool copyUsbFileToBuffer(Usb* usb, uint8_t* buffer, exFAT::File file)
{
    // Show progress bar dialog
    PS::Sce::MsgDialog::Initialize();
    PS::Sce::MsgDialogProgressBar dialog = PS::Sce::MsgDialogProgressBar("Copying PS2 ELF payload...");
    dialog.open();
    dialog.setValue(0);

    // Download ELF from network
    PS::Debug.printf("Copying %s from USB...\n", file.getName());

    uint64_t filesize = file.getSize();
    if (filesize == 0 || !usb->resetRead(file))
    {
        PS::notification("Error: %s is empty", file.getName());
        PS::Debug.printf("Error: %s is empty\n", file.getName());
        return false;
    }
    setProgress(dialog, 0, filesize);

    bool errorOccured = false;

    // Write to HDD in chunks
    uint32_t clusterSize = usb->getClusterSize();
    uint32_t updateBar = 0;
    for (uint64_t i = 0; i < filesize; i += clusterSize)
    {
        // Get remainder
        uint32_t copySize = clusterSize;
        if (i + copySize > filesize)
            copySize = filesize % copySize;

        // Read cluster from USB
        if (!usb->readNextCluster(file, buffer + i))
        {
            PS::notification("Error: Failed to read file from USB!");
            PS::Debug.printf("Failed to read file at offset %llu from USB\n", i);
            errorOccured = true;
            break;
        }

        // Update progress bar
        if (updateBar == COPY_BAR_UPDATE_FREQUENCY)
        {
            setProgress(dialog, i, filesize);
            updateBar = 0;
        }
        updateBar++;
    }
    dialog.setValue(100);
    dialog.close();
    PS::Sce::MsgDialog::Terminate();

    return !errorOccured;
}

void setProgress(PS::Sce::MsgDialogProgressBar dialog, size_t downloaded, size_t total)
{
    // Calculate percentage without float/double
    uint64_t divident = downloaded * 100;
    uint64_t percentage = 0;

    while (divident >= total)
    {
        divident -= total;
        percentage++;
    }

    dialog.setValue(percentage);
}