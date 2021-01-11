Signing The Installer Binaries
------------------------------

If the USB driver isn't signed, Windows may flag it as malware. To generate a
self-signed certificate for testing, start a Visual Studio developer command
prompt and run this:

    makecert /sr localmachine /$ individual /n "CN=<yourname>" /r /h 0 /eku "1.3.6.1.5.5.7.3.3,1.3.6.1.4.1.311.10.3.13" /e "<Expiration date in mm/dd/yyyy format>" /sv "<certname>.pvk" "<certname>.cer"
    
    < You'll be prompted to create a password, then re-enter it. >
    
    Pvk2Pfx /pvk "<certname>.pvk" /pi <Password Used Above> /spc "<certname>.cer" /pfx "<certname>.pfx"

Then, from an administrator-level command prompt, you'll need to install that
certificate locally as a trusted certificate:

    Certutil -addStore TrustedPeople "<certname>.cer"

When you're done testing, be sure to remove it:

    Certutil -store TrustedPeople
    
    < Look for the serial number of your certificate >
    
    Certutil -delStore TrustedPeople <Serial Number>

To sign things using the certificate, use the helper scripts sign-components.bat
and sign-installer.bat. You must sign the USB driver installer *before*
building the JCP installer, as the driver installer is embedded in the
installer when building it!

    sign-components.bat <certname>.pfx <Password Used Above>
    makensis jcp.nsi
    sign-installer.bat <certname>.pfx <Password Used Above>

Driver Uninstallation
---------------------

Uninstalling JCP does not uninstall the USB driver. If needed, you can either
uninstall it through device manager, or manually from an administrator-level
command prompt:

    pnputil -e

The above lists all the installed drivers. Look for one with a "Driver package
provider" of "libusb-win32" and note its "Published name" (Should be
oem<some_number>.inf). To uninstall it, ensure the skunkboard is not currently
connected to the computer, and run this:

    pnputil -d oem<the_number>.inf

On older OSes that don't have pnputil, you'll need to use devcon dp_delete (See
this article for quick links):

  https://superuser.com/questions/1002950/quick-method-to-install-devcon-exe

And for Windows XP, the only devcon builds compatible with it don't have the
necessary dp_delete command, so you just have to delete the files manuallly.
You'll have to manually search the inf files to find the right one. Look for a
line like:

    DeviceName = "Skunkboard"

Then delete these files:

    del c:\windows\inf\oem<the_number>.inf
    del c:\windows\inf\oem<the_number>.pnf
    del c:\windows\system32\drivers\libusb0.sys

And on ALL OSes, you still need to manually delete libusb0.dll if desired:

    del c:\windows\system32\libusb0.dll
