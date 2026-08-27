import QtQuick
import QtQuick.Window
import xreader

// 启动确认菜单：长按顶部中部弹出，10 秒无操作自动取消。
// 退出码：0=启动阅读器，1=取消（hello 的 run-reader.sh 据此决定是否进入应用）
Window {
    id: root
    width: Screen.width
    height: Screen.height
    visible: true
    color: "white"

    Component.onCompleted: autoCancel.restart()

    Column {
        anchors.centerIn: parent
        spacing: 70

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "remarkx 阅读器"
            font.pixelSize: 56
            font.bold: true
            color: "#1a1a1a"
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 60

            Rectangle {
                width: 420
                height: 150
                radius: 14
                color: "#ffffff"
                border.color: "#333333"
                border.width: 3
                Text {
                    anchors.centerIn: parent
                    text: "启动阅读器"
                    font.pixelSize: 44
                    font.bold: true
                    color: "#111111"
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: pageStore.menuExit(0)
                }
            }

            Rectangle {
                width: 420
                height: 150
                radius: 14
                color: "#ffffff"
                border.color: "#cccccc"
                border.width: 3
                Text {
                    anchors.centerIn: parent
                    text: "取消"
                    font.pixelSize: 44
                    color: "#888888"
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: pageStore.menuExit(1)
                }
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "10 秒后自动取消"
            font.pixelSize: 26
            color: "#aaaaaa"
        }
    }

    Timer {
        id: autoCancel
        interval: 10000
        onTriggered: pageStore.menuExit(1)
    }
}
