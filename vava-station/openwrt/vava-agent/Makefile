include $(TOPDIR)/rules.mk
PKG_NAME:=vava-agent
PKG_VERSION:=0.10.0
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_BUILD_PARALLEL:=1
include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/vava-agent
  SECTION:=net
  CATEGORY:=Network
  TITLE:=VAVA camera pairing, control and event recording agent
  DEPENDS:=+libpthread +libopenssl +@OPENSSL_WITH_DTLS +@OPENSSL_WITH_PSK +uci
  CONFLICTS:=myvava-station vava-station
endef

define Package/vava-agent/description
  Camera-facing service with dedicated AP pairing, low-power control, encrypted
  event clips and outbound CoAP/DTLS communication with your VAVA server.
  No web server, SQLite or FFmpeg on the agent. NFS support is optional.
endef

define Package/vava-agent/conffiles
/etc/config/vava-agent
/etc/vava/
/etc/vava-state/
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
endef
CMAKE_OPTIONS += -DBUILD_TESTING=OFF -DVAVA_SANITIZE=OFF \
	-DVAVA_BUILD_AGENT=ON -DVAVA_BUILD_SERVER=OFF

define Package/vava-agent/install
	$(INSTALL_DIR) $(1)/usr/bin $(1)/usr/libexec $(1)/etc/init.d $(1)/etc/config $(1)/etc/vava
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/vava-station/vava-agent $(1)/usr/bin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/vava-station/vavactl $(1)/usr/bin/
	$(INSTALL_BIN) ./files/vava-agent.init $(1)/etc/init.d/vava-agent
	$(INSTALL_CONF) ./files/vava-agent.config $(1)/etc/config/vava-agent
	$(INSTALL_BIN) ./files/vava-ap $(1)/usr/libexec/vava-ap
	$(INSTALL_BIN) ./files/vava-network $(1)/usr/bin/vava-network
	$(INSTALL_BIN) ./files/vava-agent-status $(1)/usr/libexec/vava-agent-status
	$(INSTALL_BIN) ./files/vava-agent-pair $(1)/usr/libexec/vava-agent-pair
	$(INSTALL_BIN) ./files/vava-agent-unpair $(1)/usr/libexec/vava-agent-unpair
	$(INSTALL_BIN) ./files/vava-agent-reset $(1)/usr/libexec/vava-agent-reset
	$(INSTALL_BIN) ./files/vava-agent-name $(1)/usr/libexec/vava-agent-name
	$(INSTALL_DIR) $(1)/etc/hotplug.d/ntp
	$(INSTALL_BIN) ./files/vava-ntp $(1)/etc/hotplug.d/ntp/95-vava-agent
	$(INSTALL_DIR) $(1)/usr/share/licenses/myvava-sdk/minimp4
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/myvava-sdk/THIRD_PARTY_NOTICES.txt $(PKG_BUILD_DIR)/myvava-sdk/OPENSSL-LICENSE.txt $(1)/usr/share/licenses/myvava-sdk/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/third_party/minimp4/minimp4.h $(PKG_BUILD_DIR)/third_party/minimp4/README.md $(1)/usr/share/licenses/myvava-sdk/minimp4/
endef
$(eval $(call BuildPackage,vava-agent))
