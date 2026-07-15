# Ptyxis Split Pane — 从源码编译并替换系统安装

本文档基于 `split-pane` 分支 ([https://github.com/liyingjun/ptyxis/tree/split-pane](https://github.com/liyingjun/ptyxis/tree/split-pane))，在 Docker 容器中编译构建，并替换系统自带的 Ptyxis。

## 1. 前置条件

- 宿主机已安装 Docker
- 宿主机为 Linux（GNOME 桌面环境，已安装系统版 ptyxis）

## 2. 克隆源码

```bash
git clone https://github.com/liyingjun/ptyxis.git
cd ptyxis
git checkout split-pane
```

## 3. 使用 Docker 容器编译

### 3.1 创建构建容器

```bash
docker run -d --name ptyxis-build \
  -v "$(pwd):/workspace" \
  -w /workspace \
  ubuntu:26.04 \
  sleep infinity
```

### 3.2 安装构建依赖

```bash
docker exec ptyxis-build bash -c '
apt-get update && apt-get install -y \
  build-essential \
  meson \
  ninja-build \
  pkg-config \
  libglib2.0-dev \
  libgtk-4-dev \
  libadwaita-1-dev \
  libvte-2.91-gtk4-dev \
  libportal-gtk4-dev \
  libpcre2-dev \
  gettext \
  appstream \
  git \
  libjson-glib-dev \
  desktop-file-utils
'
```

### 3.3 配置 & 编译

```bash
# 配置（如 builddir 已存在则先删除）
docker exec ptyxis-build bash -c '
cd /workspace && \
rm -rf builddir && \
meson setup builddir \
  --prefix=/usr \
  --libdir=/usr/lib \
  --libexecdir=/usr/libexec \
  --buildtype=debug
'

# 编译
docker exec ptyxis-build bash -c 'cd /workspace && ninja -C builddir'
```

> 如果仅需 debug 信息，`--buildtype=debug` 即可；如需优化构建改用 `--buildtype=release`。

### 3.4 验证编译产物

```bash
docker exec ptyxis-build bash -c '
ls -la /workspace/builddir/src/ptyxis && \
file /workspace/builddir/src/ptyxis
'
```

## 4. 替换系统 Ptyxis

以下操作在**宿主机**上执行（需要 root 权限）。

### 4.1 停止正在运行的 ptyxis

```bash
# 关闭所有 ptyxis 进程
killall ptyxis 2>/dev/null || true
```

### 4.2 备份系统原文件（可选但推荐）

```bash
sudo mkdir -p /tmp/ptyxis-backup

# 备份主程序
[ -f /usr/bin/ptyxis ] && sudo cp /usr/bin/ptyxis /tmp/ptyxis-backup/
# 备份 agent
[ -f /usr/libexec/ptyxis-agent ] && sudo cp /usr/libexec/ptyxis-agent /tmp/ptyxis-backup/
# 备份 schema
[ -f /usr/share/glib-2.0/schemas/org.gnome.Ptyxis.gschema.xml ] && \
  sudo cp /usr/share/glib-2.0/schemas/org.gnome.Ptyxis.gschema.xml /tmp/ptyxis-backup/
```

### 4.3 从容器中安装到系统

```bash
# 方式一：使用 meson install（推荐，自动处理路径）
docker exec ptyxis-build bash -c '
cd /workspace && \
DESTDIR=/workspace/builddir/staging ninja -C builddir install
'

# 将安装产物拷贝到宿主机系统目录
sudo cp -a builddir/staging/usr/bin/ptyxis /usr/bin/ptyxis
sudo cp -a builddir/staging/usr/libexec/ptyxis-agent /usr/libexec/ptyxis-agent

# 拷贝桌面文件
sudo cp -a builddir/staging/usr/share/applications/org.gnome.Ptyxis.desktop \
  /usr/share/applications/ 2>/dev/null || true

# 拷贝图标资源
sudo cp -a builddir/staging/usr/share/icons/* /usr/share/icons/ 2>/dev/null || true

# 拷贝并编译 schema
sudo cp -a builddir/staging/usr/share/glib-2.0/schemas/org.gnome.Ptyxis.gschema.xml \
  /usr/share/glib-2.0/schemas/
sudo glib-compile-schemas /usr/share/glib-2.0/schemas/

# 拷贝桌面 profile 文件
sudo cp -a builddir/staging/usr/share/ptyxis/profiles/* \
  /usr/share/ptyxis/profiles/ 2>/dev/null || true

# 拷贝 agent D-Bus service 文件
sudo cp -a builddir/staging/usr/share/dbus-1/services/* \
  /usr/share/dbus-1/services/ 2>/dev/null || true
```

### 4.4 方式二：使用 meson install 直接到系统目录

如果你想在宿主机上直接 `meson install`（需要宿主机也装了 meson）：

```bash
sudo ninja -C builddir install
sudo glib-compile-schemas /usr/share/glib-2.0/schemas/
```

> **注意**：此方式会直接覆盖系统文件，建议先做 4.2 的备份。

## 5. 一键安装脚本

将以下脚本保存为 `install.sh`，在项目根目录执行 `sudo ./install.sh`：

```bash
#!/usr/bin/env bash
set -euo pipefail

# ── 配置 ──────────────────────────────────────────────
IMAGE="ubuntu:26.04"
CONTAINER="ptyxis-build"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="builddir"
# ──────────────────────────────────────────────────────

echo "==> 1/5  准备 Docker 构建容器"
docker rm -f "$CONTAINER" 2>/dev/null || true
docker run -d --name "$CONTAINER" \
  -v "$SRC_DIR:/workspace" \
  -w /workspace \
  "$IMAGE" sleep infinity

echo "==> 2/5  安装构建依赖"
docker exec "$CONTAINER" bash -c '
apt-get update -qq && apt-get install -y -qq \
  build-essential meson ninja-build pkg-config \
  libglib2.0-dev libgtk-4-dev libadwaita-1-dev \
  libvte-2.91-gtk4-dev libportal-gtk4-dev \
  libpcre2-dev gettext appstream git \
  libjson-glib-dev desktop-file-utils
' 2>&1 | tail -1

echo "==> 3/5  编译"
docker exec "$CONTAINER" bash -c '
cd /workspace
rm -rf '"$BUILD_DIR"'
meson setup '"$BUILD_DIR"' --prefix=/usr --libdir=/usr/lib --libexecdir=/usr/libexec --buildtype=debug
ninja -C '"$BUILD_DIR"'
'

echo "==> 4/5  安装到系统目录 (需要 root)"
# 在容器内 install 到 staging 目录
docker exec "$CONTAINER" bash -c '
cd /workspace && \
DESTDIR=/workspace/'"$BUILD_DIR"'/staging ninja -C '"$BUILD_DIR"' install
'

# 拷贝到宿主机系统目录
STAGING="$SRC_DIR/$BUILD_DIR/staging"

install_file() {
    local src="$1" dst="$2"
    if [ -f "$src" ]; then
        install -D -m 644 "$src" "$dst"
        echo "    installed: $dst"
    fi
}

install_exec() {
    local src="$1" dst="$2"
    if [ -f "$src" ]; then
        install -D -m 755 "$src" "$dst"
        echo "    installed: $dst"
    fi
}

echo "    主程序 & agent"
install_exec "$STAGING/usr/bin/ptyxis"                    "/usr/bin/ptyxis"
install_exec "$STAGING/usr/libexec/ptyxis-agent"          "/usr/libexec/ptyxis-agent"

echo "    schema"
install_file  "$STAGING/usr/share/glib-2.0/schemas/org.gnome.Ptyxis.gschema.xml" \
              "/usr/share/glib-2.0/schemas/org.gnome.Ptyxis.gschema.xml"
glib-compile-schemas /usr/share/glib-2.0/schemas/

echo "    desktop file"
install_file  "$STAGING/usr/share/applications/org.gnome.Ptyxis.desktop" \
              "/usr/share/applications/org.gnome.Ptyxis.desktop" 2>/dev/null || true
install_file  "$STAGING/usr/share/applications/org.gnome.Ptyxis.Devel.desktop" \
              "/usr/share/applications/org.gnome.Ptyxis.Devel.desktop" 2>/dev/null || true

echo "    D-Bus service"
install_file  "$STAGING/usr/share/dbus-1/services/org.gnome.Ptyxis.Agent.service" \
              "/usr/share/dbus-1/services/org.gnome.Ptyxis.Agent.service" 2>/dev/null || true

echo "    图标 & palette"
cp -a "$STAGING/usr/share/icons/." /usr/share/icons/ 2>/dev/null || true
mkdir -p /usr/share/ptyxis/profiles
cp -a "$STAGING/usr/share/ptyxis/profiles/." /usr/share/ptyxis/profiles/ 2>/dev/null || true

echo "==> 5/5  清理"
docker rm -f "$CONTAINER" 2>/dev/null || true

echo ""
echo "✅ 安装完成！运行 ptyxis 即可启动。"
```

使用方法：

```bash
chmod +x install.sh
sudo ./install.sh
```

## 6. 验证安装

```bash
# 检查版本
ptyxis --version

# 检查文件
ls -la /usr/bin/ptyxis
ls -la /usr/libexec/ptyxis-agent
ls -la /usr/share/glib-2.0/schemas/org.gnome.Ptyxis.gschema.xml

# 启动
ptyxis
```

## 7. 恢复系统原版（如需要）

```bash
sudo cp /tmp/ptyxis-backup/ptyxis /usr/bin/ptyxis
sudo cp /tmp/ptyxis-backup/ptyxis-agent /usr/libexec/ptyxis-agent
sudo cp /tmp/ptyxis-backup/org.gnome.Ptyxis.gschema.xml \
  /usr/share/glib-2.0/schemas/org.gnome.Ptyxis.gschema.xml
sudo glib-compile-schemas /usr/share/glib-2.0/schemas/
```

## 8. Split Pane 快捷键

| 功能 | 快捷键 | Action |
|------|--------|--------|
| 水平分割（上下） | `Ctrl+Shift+D` | `tab.split-horizontal` |
| 垂直分割（左右） | `Ctrl+Shift+R` | `tab.split-vertical` |
| 关闭当前面板/标签 | 自定义 | `tab.close-pane` |
| 焦点上移 | `Alt+Up` | `tab.focus-pane` `'up'` |
| 焦点下移 | `Alt+Down` | `tab.focus-pane` `'down'` |
| 焦点左移 | `Alt+Left` | `tab.focus-pane` `'left'` |
| 焦点右移 | `Alt+Right` | `tab.focus-pane` `'right'` |

> `tab.close-pane` 在多 pane 时关闭当前 pane，仅剩一个 pane 时关闭整个 tab。可在 **Preferences → Shortcuts** 中自定义所有快捷键。
