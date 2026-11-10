include $(TOPDIR)/rules.mk
PKG_NAME:=vava-server
PKG_VERSION:=0.10.0
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_BUILD_PARALLEL:=1
include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/vava-server
  SECTION:=net
  CATEGORY:=Network
  TITLE:=VAVA Home independent management server
  DEPENDS:=+libpthread +libopenssl +@OPENSSL_WITH_DTLS +@OPENSSL_WITH_PSK +libsqlite3 +uci +PACKAGE_vava-server-continuous-media:libffmpeg-full
endef

define Package/vava-server/config
  config PACKAGE_vava-server-continuous-media
    bool "Continuous quality switching (FFmpeg libraries)"
    default n
    help
      Enables in-process H.264/AAC live remuxing with repeated MP4 initialization.
      Keep disabled on small flash devices; the camera-facing Agent is independent.
endef

define Package/vava-server/description
  Standalone management, accounts, events, cloud clip storage and Web service.
  Install FFmpeg separately for browser playback, snapshots and talk conversion.
  This optional server package is independent of the camera-facing vava-agent.
endef

define Package/vava-server/conffiles
/etc/config/vava-server
/etc/vava-server/
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
endef
CMAKE_OPTIONS += -DBUILD_TESTING=OFF -DVAVA_SANITIZE=OFF \
	-DVAVA_BUILD_AGENT=OFF -DVAVA_BUILD_SERVER=ON \
	-DVAVA_LIVE_AV=$(if $(CONFIG_PACKAGE_vava-server-continuous-media),ON,OFF)

define Package/vava-server/install
	$(INSTALL_DIR) $(1)/usr/bin $(1)/etc/init.d $(1)/etc/config $(1)/etc/vava-server
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/vava-server/vava-server $(1)/usr/bin/
	$(INSTALL_BIN) ./files/vava-server.init $(1)/etc/init.d/vava-server
	$(INSTALL_CONF) ./files/vava-server.config $(1)/etc/config/vava-server
	$(INSTALL_DIR) $(1)/usr/share/vava-server/web $(1)/usr/share/licenses/myvava-sdk
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/server-web/* $(1)/usr/share/vava-server/web/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/myvava-sdk/THIRD_PARTY_NOTICES.txt $(PKG_BUILD_DIR)/myvava-sdk/OPENSSL-LICENSE.txt $(1)/usr/share/licenses/myvava-sdk/
endef
$(eval $(call BuildPackage,vava-server))
