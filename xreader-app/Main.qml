import QtQuick
import QtQuick.Window
import xreader

Window {
    id: root
    width: Screen.width
    height: Screen.height
    visible: true
    color: "white"

    Image {
        id: pageImage
        anchors.fill: parent
        source: pageStore.currentFile
        cache: false
        fillMode: Image.PreserveAspectFit
    }

    InkItem {
        id: ink
        anchors.fill: parent
        Component.onCompleted: {
            setStylus(stylusObj)
            pageStore.setInk(ink)
        }
    }

    MouseArea {
        id: gest
        anchors.fill: parent
        property point pressPt
        onPressed: (mouse) => {
            idleTimer.restart()
            pressPt = Qt.point(mouse.x, mouse.y)
        }
        onReleased: (mouse) => {
            const dx = mouse.x - pressPt.x
            const dy = mouse.y - pressPt.y
            const adx = Math.abs(dx)
            const ady = Math.abs(dy)
            if (adx < 90 && ady < 90)
                return
            if (adx > ady)
                dx < 0 ? pageStore.next() : pageStore.prev()
            else
                dy < 0 ? pageStore.refresh() : pageStore.quit()
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
    MouseArea {
        property point p0
        x: 0
        y: 0
        width: parent.width
        height: 140
        enabled: stylusObj.calibrated
        onPressed: (m) => {
            idleTimer.restart()
            p0 = Qt.point(m.x, m.y)
        }
        onReleased: (m) => {
            if (m.y - p0.y >= 90)
                pageStore.quit()
        }
    }
    MouseArea {
        property point p1
        x: 0
        y: parent.height - 140
        width: parent.width
        height: 140
        enabled: stylusObj.calibrated
        onPressed: (m) => {
            idleTimer.restart()
            p1 = Qt.point(m.x, m.y)
        }
        onReleased: (m) => {
            if (p1.y - m.y >= 90)
                pageStore.refresh()
        }
    }
}
