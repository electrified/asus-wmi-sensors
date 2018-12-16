_pkgbase=asus-wmi-sensors
pkgname=asus-wmi-sensors-dkms-git
pkgver=25.4dfdbdb
pkgrel=1
pkgdesc="Linux sensors driver for ASUS motherboards with WMI sensors interface"
arch=('x86_64' 'i686')
url="https://github.com/electrified/asus-wmi-sensors"
license=('GPL')
depends=('dkms')
provides=('asus-wmi-sensors')

source=("$_pkgbase::git+https://github.com/electrified/asus-wmi-sensors.git")

sha256sums=('SKIP')

pkgver() {
  cd "$srcdir/$_pkgbase"
  printf "%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

prepare() {
  cd "$srcdir/$_pkgbase"
}

package() {
  cd "$srcdir/$_pkgbase"

  install -d "${pkgdir}"/usr/src/${_pkgbase}-${pkgver}/
  cp -r ${srcdir}/${_pkgbase}/* "${pkgdir}"/usr/src/${_pkgbase}-${pkgver}/

  sed -e "s/@_PKGBASE@/${_pkgbase}/" \
    -e "s/@PKGVER@/${pkgver}/" \
    -i "${pkgdir}"/usr/src/${_pkgbase}-${pkgver}/dkms.conf

  install -Dm644 ${srcdir}/${_pkgbase}/asus-wmi-sensors.conf "${pkgdir}"/usr/lib/depmod.d/asus-wmi-sensors.conf

  install -Dm644 ${srcdir}/${_pkgbase}/module-load.conf "${pkgdir}"/etc/modules-load.d/asus-wmi-sensors.conf
}
