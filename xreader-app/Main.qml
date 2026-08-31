import QtQuick
import QtQuick.Window
import xreader

Window {
    id: root
    width: Screen.width
    height: Screen.height
    visible: true
    color: "white"
    // 全屏刷新计数：每 3 页触发一次强制刷新
    property int refreshCount: 0

    Image {
        id: pageImage
        anchors.fill: parent
        source: pageStore.currentFile
        cache: false
        fillMode: Image.PreserveAspectFit
        // 每 5 个不同页面全屏强制刷新一次，清除墨水屏残影（每次都刷太慢）
        property int lastCountedKey: -1
        onStatusChanged: {
            if (status !== Image.Ready)
                return
            if (pageStore.pageKey === pageImage.lastCountedKey)
                return
            pageImage.lastCountedKey = pageStore.pageKey
            root.refreshCount += 1
            if (root.refreshCount % 5 === 0)
                pageStore.requestFullRefresh()
        }
    }

    // 图片懒加载层：底层文字位图只画占位框，这里负责贴图/状态文案
    Item {
        id: photoLayer
        anchors.fill: parent
        Repeater {
            model: pageStore.imageSlots
            delegate: Item {
                x: modelData.x
                y: modelData.y
                width: modelData.w
                height: modelData.h
                Rectangle {
                    anchors.fill: parent
                    color: "#ececec"
                    border.color: "#d9d9d9"
                    border.width: 2
                }
                Image {
                    anchors.fill: parent
                    source: modelData.ready ? ("file://" + modelData.path) : ""
                    fillMode: Image.PreserveAspectFit
                    cache: false
                    visible: modelData.ready
                }
                // 视频封面：中心圆形播放按钮（仅用于辨识视频，实际不支持播放）
                Canvas {
                    anchors.centerIn: parent
                    width: 76
                    height: 76
                    visible: modelData.video && modelData.ready
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        ctx.fillStyle = "rgba(0, 0, 0, 0.55)"
                        ctx.beginPath()
                        ctx.arc(width / 2, height / 2, width / 2 - 2, 0, Math.PI * 2)
                        ctx.fill()
                        ctx.fillStyle = "#ffffff"
                        ctx.beginPath()
                        ctx.moveTo(width / 2 - 13, height / 2 - 22)
                        ctx.lineTo(width / 2 - 13, height / 2 + 22)
                        ctx.lineTo(width / 2 + 25, height / 2)
                        ctx.closePath()
                        ctx.fill()
                    }
                }
                Text {
                    anchors.centerIn: parent
                    text: modelData.ready ? "" : (modelData.failed ? "加载失败" : "图片加载中")
                    font.pixelSize: 26
                    color: "#8a8a8a"
                    visible: !modelData.ready
                }
                Text {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 8
                    text: modelData.nMedia > 1 ? ("共 " + modelData.nMedia + " 图") : ""
                    font.pixelSize: 20
                    font.bold: true
                    color: "white"
                    style: Text.Outline
                    styleColor: "#333333"
                    visible: modelData.nMedia > 1
                }
            }
        }
    }

    InkItem {
        id: ink
        anchors.fill: parent
        Component.onCompleted: {
            setStylus(stylusObj)
            pageStore.setInk(ink)
        }
    }

    // 全屏左右滑翻页（上下滑移交给顶部/底部边缘条，避免中间误触）
    // 防误触：按下与抬起都要确认笔完全空闲（penActive 为假且距最后一次笔活动
    // >=500ms），堵住"手掌先落屏、笔后开始写、手掌抬起时误触"的竞态；
    // 按压过长且位移仍很小判定为手掌托屏，不作 tap。
    property int gestureMinIdleMs: 500
    property int palmDwellMs: 700
    MouseArea {
        id: gest
        anchors.fill: parent
        property point pressPt
        property int pressMs: 0
        property bool armed: false
        onPressed: (mouse) => {
            idleTimer.restart()
            // 手写笔在用/最近写过字：手指触摸一律不算手势
            if (root.penBusy())
                return
            armed = true
            pressPt = Qt.point(mouse.x, mouse.y)
            pressMs = Date.now()
        }
        onReleased: (mouse) => {
            idleTimer.restart()
            if (!armed)
                return
            armed = false
            // 抬起时也要复查：手掌落屏后笔可能已开始写，此时抬手不得触发任何手势
            if (root.penBusy())
                return
            const dx = mouse.x - pressPt.x
            const dy = mouse.y - pressPt.y
            const adx = Math.abs(dx)
            const ady = Math.abs(dy)
            // 手掌托屏：按压过长且位移未出 tap 区 → 忽略
            if (Date.now() - pressMs > root.palmDwellMs && adx < 90 && ady < 90)
                return
            // 手指短点不再点开任何内容（图片/全文都改由笔点按打开，见 penTap）
            // 手指只负责滑动手势
            if (adx < 90 && ady < 90)
                return
            // 有效距离 + 主方向水平，移动一点点不算
            if (adx < 90 || adx <= ady)
                return
            dx < 0 ? pageStore.next() : pageStore.prev()
        }
    }

    Text {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 30
        text: pageStore.bookLabel
        font.pixelSize: 28
        color: "#999"
    }

    Rectangle {
        visible: pageStore.loading
        anchors.fill: parent
        color: "white"
        Text {
            anchors.centerIn: parent
            text: pageStore.status
            font.pixelSize: 34
            color: "#333"
        }
    }

    Rectangle {
        visible: pageStore.error.length > 0
        anchors.fill: parent
        color: "white"
        Column {
            width: root.width - 200
            anchors.centerIn: parent
            spacing: 30
            Text {
                width: parent.width
                text: pageStore.error
                font.pixelSize: 30
                color: "#555"
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "重试"
                font.pixelSize: 34
                font.bold: true
                color: "#111"
                MouseArea {
                    anchors.fill: parent
                    onClicked: pageStore.retry()
                }
            }
        }
    }

    Rectangle {
        id: calib
        visible: !stylusObj.calibrated
        anchors.fill: parent
        color: "white"

        property var pts: []
        property var targets: [
            {x: 200, y: 320}, {x: 1204, y: 320}, {x: 702, y: 900},
            {x: 200, y: 1480}, {x: 1204, y: 1480}]
        property int next: 0

        Text {
            text: "请用笔依次点击 1 → 5 的十字中心"
            font.pixelSize: 30
            color: "#333"
            anchors.horizontalCenter: parent.horizontalCenter
            y: 90
        }
        Text {
            text: "完成后点击右下角“跳过”可直接浏览"
            font.pixelSize: 24
            color: "#888"
            anchors.horizontalCenter: parent.horizontalCenter
            y: 150
        }

        Connections {
            target: stylusObj
            function onRawPenDown(rx, ry) {
                if (!calib.visible)
                    return;
                if (calib.next >= calib.targets.length)
                    return;
                calib.pts.push({rx: rx, ry: ry});
                calib.next += 1;
                if (calib.next >= calib.targets.length)
                    calib.finish(false);
            }
        }

        Repeater {
            model: calib.targets.length
            delegate: Item {
                x: calib.targets[index].x - 60
                y: calib.targets[index].y - 60
                width: 120
                height: 120
                Rectangle {
                    anchors.centerIn: parent
                    width: 120
                    height: 2
                    color: "#999"
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 2
                    height: 120
                    color: "#999"
                }
                Text {
                    anchors.centerIn: parent
                    text: index + 1
                    font.pixelSize: 44
                    font.bold: true
                    color: "#111"
                }
            }
        }

        Text {
            text: "跳过"
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 30
            font.pixelSize: 32
            font.bold: true
            color: "#444"
            MouseArea {
                anchors.fill: parent
                onClicked: calib.finish(true)
            }
        }

        function finish(skip) {
            if (!skip && calib.pts.length === 5) {
                var r = solve(calib.pts, calib.targets);
                stylusObj.setCalib(r.a, r.b, r.c, r.d, r.e, r.f);
                stylusObj.saveCalib(baseDir + "/calib.json");
            }
            calib.visible = false;
        }

        function solve(raw, tgt) {
            var n = raw.length;
            var srx = 0, sry = 0, srx2 = 0, sry2 = 0, srxy = 0;
            var stx = 0, stxrx = 0, stxry = 0;
            var sty = 0, styrx = 0, styry = 0;
            for (var i = 0; i < n; i++) {
                var rx = raw[i].rx, ry = raw[i].ry;
                var tx = tgt[i].x, ty = tgt[i].y;
                srx += rx; sry += ry; srx2 += rx * rx; sry2 += ry * ry; srxy += rx * ry;
                stx += tx; stxrx += tx * rx; stxry += tx * ry;
                sty += ty; styrx += ty * rx; styry += ty * ry;
            }
            var M = [[srx2, srxy, srx], [srxy, sry2, sry], [srx, sry, n]];
            var vx = [stxrx, stxry, stx];
            var vy = [styrx, styry, sty];
            var ix = solve3(M, vx), iy = solve3(M, vy);
            return {a: ix[0], b: ix[1], c: ix[2], d: iy[0], e: iy[1], f: iy[2]};
        }

        function solve3(M, v) {
            var A = M.map(function(r) { return r.slice(); });
            var b = v.slice();
            for (var col = 0; col < 3; col++) {
                var piv = col;
                for (var r = col + 1; r < 3; r++)
                    if (Math.abs(A[r][col]) > Math.abs(A[piv][col])) piv = r;
                var t = A[col]; A[col] = A[piv]; A[piv] = t;
                var tb = b[col]; b[col] = b[piv]; b[piv] = tb;
                for (var r2 = col + 1; r2 < 3; r2++) {
                    var f = A[r2][col] / A[col][col];
                    for (var c2 = col; c2 < 3; c2++) A[r2][c2] -= f * A[col][c2];
                    b[r2] -= f * b[col];
                }
            }
            var x = [0, 0, 0];
            for (var i = 2; i >= 0; i--) {
                var s = b[i];
                for (var j = i + 1; j < 3; j++) s -= A[i][j] * x[j];
                 x[i] = s / A[i][i];
             }
             return x;
         }
     }

    // 休眠提示层：电源短按后显示，留足墨水屏刷新时间再挂起
    Rectangle {
        id: sleepOverlay
        visible: false
        anchors.fill: parent
        color: "white"

        Column {
            anchors.centerIn: parent
            spacing: 40

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 30

                // 电源图标（圆环+竖线），避免依赖系统字体的特殊字形
                Canvas {
                    width: 64
                    height: 64
                    anchors.verticalCenter: parent.verticalCenter
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.strokeStyle = "#1a1a1a"
                        ctx.lineWidth = 7
                        ctx.lineCap = "round"
                        var cx = width / 2, cy = height / 2 + 6, r = 22
                        ctx.beginPath()
                        ctx.arc(cx, cy, r, -Math.PI / 2 + 0.55,
                                -Math.PI / 2 - 0.55, false)
                        ctx.stroke()
                        ctx.beginPath()
                        ctx.moveTo(cx, cy - r - 8)
                        ctx.lineTo(cx, cy - r + 10)
                        ctx.stroke()
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "正在休眠"
                    font.pixelSize: 64
                    font.bold: true
                    color: "#1a1a1a"
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "即将自动休眠，按电源键唤醒，阅读进度已保存"
                font.pixelSize: 30
                color: "#666666"
            }
        }
    }

    Connections {
        target: powerKeyObj
        function onShortPress() {
            if (sleepOverlay.visible)
                return
            sleepOverlay.visible = true
            suspendDelay.restart()
        }
    }

    // 空闲自动休眠：5 分钟无手势/笔/页面活动 → 与电源键相同的休眠流程
    Timer {
        id: idleTimer
        interval: 300000
        running: true
        onTriggered: {
            if (sleepOverlay.visible)
                return
            sleepOverlay.visible = true
            suspendDelay.restart()
        }
    }
    Connections {
        target: stylusObj
        function onPenDown(x, y, p) { idleTimer.restart() }
        function onPenMove(x, y, p) { idleTimer.restart() }
        function onEraserDown(x, y, p) { idleTimer.restart() }
        function onEraserMove(x, y, p) { idleTimer.restart() }
        // 笔点（短按+微移）：命中图片槽位 → 全屏看图；命中"显示全文"按钮
        // 热区 → 全屏看全文；都没命中的笔点/一切笔划照旧只是笔迹。
        // 图片优先（与原手指路径一致，槽位在卡片内先于按钮热区判定）
        function onPenTap(x, y) {
            idleTimer.restart()
            // 校准/全屏看图/全屏全文期间笔点不触发（全屏里笔只写笔迹）
            if (calib.visible || fullscreen.visible || textFs.visible)
                return
            var idx = pageStore.hitSlot(x, y)
            if (idx >= 0) {
                ink.eraseLastStroke()   // 抹掉笔点的墨点，图上不残留
                fullscreen.open(idx)
                return
            }
            var tid = pageStore.hitFullText(x, y)
            if (tid.length > 0) {
                ink.eraseLastStroke()
                textFs.open(tid)
            }
        }
    }
    Connections {
        target: pageStore
        function onStateChanged() { idleTimer.restart() }
    }

    Timer {
        id: suspendDelay
        interval: 1600          // 等提示刷上墨水屏
        onTriggered: {
            pageStore.suspendNow()
            wakeClear.interval = 6000
            wakeClear.restart()
        }
    }

    Timer {
        id: wakeClear
        interval: 6000
        onTriggered: {
            sleepOverlay.visible = false
            idleTimer.restart()
        }
    }

    // 置顶边缘手势：任何时候可用（含加载/错误/任意页面状态）
    // 顶部从上往下滑 → 退出；底部从下往上滑 → 刷新
    // 只认从边缘开始的、距离足够的、手指（非手写笔）的垂直滑动
    // 防误触：按下与抬起都要求笔完全空闲，手掌/手指贴屏写字时不会触发
    function penBusy() {
        return stylusObj.penActive || stylusObj.penIdleMs() < root.gestureMinIdleMs
    }
    MouseArea {
        property point p0
        property bool armed: false
        x: 0
        y: 0
        width: parent.width
        height: 140
        enabled: stylusObj.calibrated
        onPressed: (m) => {
            idleTimer.restart()
            if (root.penBusy())
                return
            armed = true
            p0 = Qt.point(m.x, m.y)
        }
        onReleased: (m) => {
            idleTimer.restart()
            if (!armed)
                return
            armed = false
            if (root.penBusy())
                return
            const ady = m.y - p0.y
            const adx = Math.abs(m.x - p0.x)
            if (ady < 90 || ady <= adx)
                return
            pageStore.quit()
        }
    }
    MouseArea {
        property point p1
        property bool armed: false
        x: 0
        y: parent.height - 140
        width: parent.width
        height: 140
        enabled: stylusObj.calibrated
        onPressed: (m) => {
            idleTimer.restart()
            if (root.penBusy())
                return
            armed = true
            p1 = Qt.point(m.x, m.y)
        }
        onReleased: (m) => {
            idleTimer.restart()
            if (!armed)
                return
            armed = false
            if (root.penBusy())
                return
            const ady = p1.y - m.y
            const adx = Math.abs(m.x - p1.x)
            if (ady < 90 || ady <= adx)
                return
            pageStore.refresh()
        }
    }

    // 图片全屏查看：点按任意处/顶部下滑关闭，左右滑在槽位媒体间切换
    Item {
        id: fullscreen
        visible: false
        anchors.fill: parent
        z: 200

        property int slotIndex: -1
        property var files: []
        property int currentIdx: 0
        property string currentPath: ""
        property string label: ""

        function updateCurrent() {
            if (files.length === 0) {
                currentPath = ""
                label = "图片加载中…"
            } else {
                if (currentIdx >= files.length)
                    currentIdx = 0
                currentPath = "file://" + files[currentIdx]
                label = "图片 " + (currentIdx + 1) + "/" + files.length
            }
        }
        function open(idx) {
            slotIndex = idx
            files = pageStore.slotFiles(idx)
            currentIdx = 0
            updateCurrent()
            visible = true
            pageStore.requestFullRefresh()
        }
        function close() {
            visible = false
            pageStore.requestFullRefresh()
        }
        function nextImg() {
            if (files.length > 1) {
                currentIdx = (currentIdx + 1) % files.length
                updateCurrent()
                pageStore.requestFullRefresh()
            }
        }
        function prevImg() {
            if (files.length > 1) {
                currentIdx = (currentIdx - 1 + files.length) % files.length
                updateCurrent()
                pageStore.requestFullRefresh()
            }
        }

        // 媒体懒加载就绪时自动刷新全屏内容
        Connections {
            target: pageStore
            function onImageSlotsChanged() {
                if (fullscreen.visible) {
                    fullscreen.files = pageStore.slotFiles(fullscreen.slotIndex)
                    fullscreen.updateCurrent()
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "white"
        }
        Image {
            anchors.fill: parent
            anchors.margins: 40
            source: fullscreen.currentPath
            fillMode: Image.PreserveAspectFit
            cache: false
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 40
            text: fullscreen.label
            font.pixelSize: 30
            color: "#888888"
        }
        Text {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 30
            text: "点按关闭 · 左右滑切换"
            font.pixelSize: 24
            color: "#aaaaaa"
        }

        MouseArea {
            anchors.fill: parent
            property point p0
            property bool armed: false
            onPressed: (m) => {
                idleTimer.restart()
                // 写笔记期间（手掌/手指贴屏）不得误关全屏图
                if (root.penBusy())
                    return
                armed = true
                p0 = Qt.point(m.x, m.y)
            }
            onReleased: (m) => {
                if (!armed)
                    return
                armed = false
                if (root.penBusy())
                    return
                const dx = m.x - p0.x
                const dy = m.y - p0.y
                if (Math.abs(dx) < 60 && Math.abs(dy) < 60) {
                    fullscreen.close()
                    return
                }
                if (Math.abs(dx) > Math.abs(dy)) {
                    dx < 0 ? fullscreen.nextImg() : fullscreen.prevImg()
                } else if (dy > 90) {
                    fullscreen.close()   // 顶部下滑关闭
                }
            }
        }
    }

    // 全文全屏阅读：笔点按卡片上"显示全文"按钮打开；左右滑在长文页间切换
    Item {
        id: textFs
        visible: false
        anchors.fill: parent
        z: 200

        property string tweetId: ""
        property int page: 0
        property int pages: 1
        property string sourceUrl: ""

        function open(tid) {
            tweetId = tid
            page = 0
            pages = Math.max(1, pageStore.fullTextPages(tid))
            sourceUrl = "image://pages/text/" + tweetId + "/" + page
            visible = true
            pageStore.requestFullRefresh()
        }
        function close() {
            visible = false
            tweetId = ""
            pageStore.requestFullRefresh()
        }
        function nextPage() {
            if (page + 1 < pages) {
                page += 1
                sourceUrl = "image://pages/text/" + tweetId + "/" + page
                pageStore.requestFullRefresh()
            }
        }
        function prevPage() {
            if (page > 0) {
                page -= 1
                sourceUrl = "image://pages/text/" + tweetId + "/" + page
                pageStore.requestFullRefresh()
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "white"
        }
        Image {
            anchors.fill: parent
            source: textFs.sourceUrl
            cache: false
            fillMode: Image.PreserveAspectFit
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 20
            text: textFs.pages > 1 ? ("第 " + (textFs.page + 1) + " 页 · 共 "
                                      + textFs.pages + " 页") : ""
            font.pixelSize: 26
            color: "#888888"
        }
        Text {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 30
            text: "点按关闭 · 左右滑翻页"
            font.pixelSize: 24
            color: "#aaaaaa"
        }

        MouseArea {
            anchors.fill: parent
            property point p0
            property bool armed: false
            onPressed: (m) => {
                idleTimer.restart()
                // 写笔记期间（手掌/手指贴屏）不得误关全屏全文
                if (root.penBusy())
                    return
                armed = true
                p0 = Qt.point(m.x, m.y)
            }
            onReleased: (m) => {
                if (!armed)
                    return
                armed = false
                if (root.penBusy())
                    return
                const dx = m.x - p0.x
                const dy = m.y - p0.y
                if (Math.abs(dx) < 60 && Math.abs(dy) < 60) {
                    textFs.close()
                    return
                }
                if (Math.abs(dx) > Math.abs(dy)) {
                    dx < 0 ? textFs.nextPage() : textFs.prevPage()
                } else if (dy > 90) {
                    textFs.close()   // 顶部下滑关闭
                }
            }
        }
    }
}
