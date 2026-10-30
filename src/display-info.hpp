#pragma once

#include <QRect>
#include <QString>

struct DisplayInfo {
	QString name;
	QString description;
	QRect geometry;
	int scale = 1;
	bool sameCaptureLayout(const DisplayInfo &other) const
	{
		return name == other.name && geometry == other.geometry && scale == other.scale;
	}
	bool operator==(const DisplayInfo &other) const
	{
		return sameCaptureLayout(other) && description == other.description;
	}
};
