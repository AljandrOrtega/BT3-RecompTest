#pragma once

#include <QWidget>

// About tab (last): project identity, credits and third-party attributions.
class AboutTab : public QWidget
{
    Q_OBJECT
public:
    explicit AboutTab(QWidget *parent = nullptr);
};
