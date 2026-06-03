#!/bin/bash

sudo cp -R tobii_usb_service/etc/* /etc/
sudo cp -R tobii_usb_service/usr/* /usr/
sudo rm -rf /usr/share/tobii_engine
tar -xzf tobii_engine/usr/share/tobii_engine.tar.gz -C tobii_engine/usr/share/
sudo cp -R tobii_engine/etc/* /etc/
sudo cp -R tobii_engine/usr/* /usr/
sudo systemctl daemon-reload
sudo systemctl start tobiiusb.service
sudo systemctl start tobii_engine.service
sudo systemctl enable tobiiusb.service
sudo systemctl enable tobii_engine.service
sudo mkdir /usr/lib/tobii
sudo cp -pR lib/lib/x64/*.so /usr/lib/tobii/
sudo cp ./tobii.conf /etc/ld.so.conf.d/
sudo mkdir /usr/include/tobii
sudo cp -R lib/include/tobii/* /usr/include/tobii
sudo cp ./tobii.conf /etc/ld.so.conf.d/
sudo ldconfig

# tobii_engine requires libsqlcipher.so.0
# create symlink if not already present
if [ ! -f /usr/lib/x86_64-linux-gnu/libsqlcipher.so.0 ]; then
    if [ -f /usr/lib/x86_64-linux-gnu/libsqlcipher.so.2 ]; then
        sudo ln -s /usr/lib/x86_64-linux-gnu/libsqlcipher.so.2 \
                   /usr/lib/x86_64-linux-gnu/libsqlcipher.so.0
        echo "Created libsqlcipher.so.0 symlink"
    else
        echo "WARNING: libsqlcipher not found"
        echo "Run: sudo apt install sqlcipher"
        echo "Then retry: sudo ln -s /usr/lib/x86_64-linux-gnu/libsqlcipher.so.2 /usr/lib/x86_64-linux-gnu/libsqlcipher.so.0"
    fi
fi

sudo systemctl restart tobii_engine.service
echo "Install complete"
