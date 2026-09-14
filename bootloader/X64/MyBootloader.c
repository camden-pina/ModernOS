//
// MyBootloader.c
// A minimal UEFI bootloader using EDK2
//

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/BaseLib.h>

// UEFI applications built with EDK2 use the UefiMain() entry point.
EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
    EFI_STATUS Status;

    // Print a welcome message to the UEFI console.
    Print(L"Hello from MyBootloader built with EDK2!\n");

    //
    // Example: Use UEFI Boot Services to locate the Simple File System Protocol
    // on the device from which this image was loaded. (You can later use it to
    // load a kernel or other files.)
    //
    EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
    Status = gBS->HandleProtocol(
                   ImageHandle,
                   &gEfiLoadedImageProtocolGuid,
                   (VOID **)&LoadedImage
                   );
    if (EFI_ERROR(Status)) {
        Print(L"ERROR: Failed to retrieve Loaded Image Protocol: %r\n", Status);
        return Status;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
    Status = gBS->HandleProtocol(
                   LoadedImage->DeviceHandle,
                   &gEfiSimpleFileSystemProtocolGuid,
                   (VOID **)&FileSystem
                   );
    if (EFI_ERROR(Status)) {
        Print(L"ERROR: Failed to retrieve Simple File System Protocol: %r\n", Status);
        return Status;
    }

    EFI_FILE_PROTOCOL *Root;
    Status = FileSystem->OpenVolume(FileSystem, &Root);
    if (EFI_ERROR(Status)) {
        Print(L"ERROR: Unable to open volume: %r\n", Status);
        return Status;
    }

    // (Optional) You can now use 'Root' to open files, list directories, etc.

    // Wait for a key press before exiting.
    Print(L"Press any key to exit...\n");
    UINTN Index;
    gBS->WaitForEvent(1, &SystemTable->ConIn->WaitForKey, &Index);
    EFI_INPUT_KEY Key;
    SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &Key);

    return EFI_SUCCESS;
}

