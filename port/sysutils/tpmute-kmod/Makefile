PORTNAME=	tpmute
DISTVERSIONPREFIX=	v
DISTVERSION=	0.1.0
CATEGORIES=	sysutils
PKGNAMESUFFIX=	-kmod

MAINTAINER=	wugq.dev@gmail.com
COMMENT=	Kernel module sending a devd event on ThinkPad EC speaker mute
WWW=		https://github.com/wugq/tpmute-kmod

LICENSE=	BSD2CLAUSE
LICENSE_FILE=	${WRKSRC}/LICENSE

ONLY_FOR_ARCHS=	amd64 i386
ONLY_FOR_ARCHS_REASON=	ThinkPad hardware (acpi_ibm)

USES=		kmod uidfix
USE_GITHUB=	yes
GH_ACCOUNT=	wugq
GH_PROJECT=	tpmute-kmod

PLIST_FILES=	${KMODDIR}/tpmute.ko

.include <bsd.port.mk>
