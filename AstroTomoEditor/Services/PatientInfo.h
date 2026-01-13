#pragma once
#include <QString>

struct PatientInfo
{
	QString patientName;
	QString patientId;
	QString sex;
	QString birthDate;	
	QString Mode;
	QString Description;
	QString Sequence;

	QString RepetitionTime;
	QString EchoTime;
	QString InversionTime;
	QString FlipAngle;
	QString ScanningSequence;
	QString ImageType;

	QString MagneticFieldStrength;
	QString ScanOptions;
	QString Manufacturer;

	QString ContrastBolusAgent;
	QString ContrastBolusStartTime;

	QString DicomPath;
	QString SeriesNumber;
};
