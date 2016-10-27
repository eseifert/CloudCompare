//##########################################################################
//#                                                                        #
//#                              CLOUDCOMPARE                              #
//#                                                                        #
//#  This program is free software; you can redistribute it and/or modify  #
//#  it under the terms of the GNU General Public License as published by  #
//#  the Free Software Foundation; version 2 or later of the License.      #
//#                                                                        #
//#  This program is distributed in the hope that it will be useful,       #
//#  but WITHOUT ANY WARRANTY; without even the implied warranty of        #
//#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the          #
//#  GNU General Public License for more details.                          #
//#                                                                        #
//#          COPYRIGHT: EDF R&D / TELECOM ParisTech (ENST-TSI)             #
//#                                                                        #
//##########################################################################

#include "SinusxFilter.h"

//qCC_db
#include <ccLog.h>
#include <ccPolyline.h>
#include <ccPointCloud.h>

//Qt
#include <QFile>
#include <QTextStream>
#include <QDialog>

//System
#include <string.h>

bool SinusxFilter::canSave(CC_CLASS_ENUM type, bool& multiple, bool& exclusive) const
{
	if (type == CC_TYPES::POLY_LINE)
	{
		multiple = true;
		exclusive = true;
		return true;
	}
	return false;
}

bool SinusxFilter::canLoadExtension(QString upperCaseExt) const
{
	return (	upperCaseExt == "SX"
			||	upperCaseExt == "SINUSX" );
}

QString MakeSinusxName(QString name)
{
	//no space characters
	name.replace(' ','_');
	
	return name;
}

CC_FILE_ERROR SinusxFilter::saveToFile(ccHObject* entity, QString filename, SaveParameters& parameters)
{
	if (!entity || filename.isEmpty())
		return CC_FERR_BAD_ARGUMENT;

	//look for polylines only
	std::vector<ccPolyline*> profiles;
	try
	{
		if (entity->isA(CC_TYPES::POLY_LINE))
		{
			profiles.push_back(static_cast<ccPolyline*>(entity));
		}
		else if (entity->isA(CC_TYPES::HIERARCHY_OBJECT))
		{
			for (unsigned i=0; i<entity->getChildrenNumber(); ++i)
				if (entity->getChild(i) && entity->getChild(i)->isA(CC_TYPES::POLY_LINE))
					profiles.push_back(static_cast<ccPolyline*>(entity->getChild(i)));
		}
	}
	catch (const std::bad_alloc&)
	{
		return CC_FERR_NOT_ENOUGH_MEMORY;
	}

	if (profiles.empty())
		return CC_FERR_NO_SAVE;

	//open ASCII file for writing
	QFile file(filename);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
		return CC_FERR_WRITING;

	QTextStream outFile(&file);
	static const int s_precision = 12;
	outFile.setRealNumberNotation(QTextStream::FixedNotation);
	outFile.setRealNumberPrecision(s_precision);

	CC_FILE_ERROR result = CC_FERR_NO_SAVE;

	//write header
	outFile << "C Generated by CloudCompare" << endl;

	//for each profile
	for (size_t i = 0; i < profiles.size(); ++i)
	{
		ccPolyline* poly = profiles[i];
		unsigned vertCount = poly ? poly->size() : 0;
		if (vertCount < 2)
		{
			//invalid size
			ccLog::Warning(QString("[Sinusx] Polyline '%1' does not have enough vertices").arg(poly->getName()));
			continue;
		}

		bool is2D = poly->is2DMode();

		int upDir = 2;
		if (is2D && poly->hasMetaData(ccPolyline::MetaKeyUpDir()))
		{
			bool ok;
			upDir = poly->getMetaData(ccPolyline::MetaKeyUpDir()).toInt(&ok);
			if (!ok)
				upDir = 2; //restore default value
		}

		//new block
		outFile << "B S" << endl; //B + curve type (S = set of 3D points)
		outFile << "CN " << poly->getName() << endl; //CN + name
		outFile << "CP 1 " << (poly->isClosed() ? 1 : 0) << endl; //CP + isLinked + isClosed
		outFile << "CP " << (upDir == 2 ? 0 : (upDir == 1 ? 2 : 1)) << endl; //base plane: 0 = (XY), 1 = (YZ), 2 = (ZX)

		for (unsigned j = 0; j < vertCount; ++j)
		{
			const CCVector3* P = poly->getPoint(j);
			CCVector3d Pg = poly->toGlobal3d(*P);

			for (unsigned k = 0; k < 3; ++k)
			{
				outFile << " ";
				if (P->u[k] >= 0)
					outFile << "+";
				outFile << QString::number(Pg.u[k], 'E', s_precision);
			}
			outFile << " A" << endl;
		}

		result = CC_FERR_NO_ERROR;
	}

	file.close();

	return result;
}

enum CurveType { INVALID = -1, CUREV_S = 0, CURVE_P = 1, CURVE_N = 2, CURVE_C = 3 };
const QChar SHORTCUT[] = { 'S', 'P', 'N', 'C' };

CC_FILE_ERROR SinusxFilter::loadFile(QString filename, ccHObject& container, LoadParameters& parameters)
{
	//open file
	QFile file(filename);
	if (!file.open(QFile::ReadOnly))
		return CC_FERR_READING;
	QTextStream stream(&file);

	QString currentLine("C");
	ccPolyline* currentPoly = 0;
	ccPointCloud* currentVertices = 0;
	unsigned lineNumber = 0;
	CurveType curveType = INVALID;
	unsigned cpIndex = 0;
	CC_FILE_ERROR result = CC_FERR_NO_ERROR;
	CCVector3d Pshift(0,0,0);
	bool firstVertex = true;

	while (!currentLine.isEmpty() && file.error() == QFile::NoError)
	{
		currentLine = stream.readLine();
		++lineNumber;

		if (currentLine.startsWith("C "))
		{
			//ignore comments
			continue;
		}
		else if (currentLine.startsWith("B"))
		{
			//new block
			if (currentPoly)
			{
				if (	currentVertices
					&&	currentVertices->size() != 0
					&& currentVertices->resize(currentVertices->size())
					&&	currentPoly->addPointIndex(0,currentVertices->size()) )
				{
					container.addChild(currentPoly);
				}
				else
				{
					delete currentPoly;
				}
				currentPoly = 0;
				currentVertices = 0;

			}
			//read type
			QStringList tokens = currentLine.split(QRegExp("\\s+"),QString::SkipEmptyParts);
			if (tokens.size() < 2 || tokens[1].length() > 1)
			{
				ccLog::Warning(QString("[SinusX] Line %1 is corrupted").arg(lineNumber));
				result = CC_FERR_MALFORMED_FILE;
				continue;
			}
			QChar curveTypeChar = tokens[1].at(0);
			curveType = INVALID;
			if (curveTypeChar == SHORTCUT[CUREV_S])
				curveType = CUREV_S;
			else if (curveTypeChar == SHORTCUT[CURVE_P])
				curveType = CURVE_P;
			else if (curveTypeChar == SHORTCUT[CURVE_N])
				curveType = CURVE_N;
			else if (curveTypeChar == SHORTCUT[CURVE_C])
				curveType = CURVE_C;

			if (curveType == INVALID)
			{
				ccLog::Warning(QString("[SinusX] Unhandled curve type '%1' on line '%2'!").arg(curveTypeChar).arg(lineNumber));
				result = CC_FERR_MALFORMED_FILE;
				continue;
			}

			//TODO: what about the local coordinate system and scale?! (7 last values)
			if (tokens.size() > 7)
			{
			}

			//block is ready
			currentVertices = new ccPointCloud("vertices");
			currentPoly = new ccPolyline(currentVertices);
			currentPoly->addChild(currentVertices);
			currentVertices->setEnabled(false);
			cpIndex = 0;
		}
		else if (currentPoly)
		{
			if (currentLine.startsWith("CN"))
			{
				if (currentLine.length() > 3)
				{
					QString name = currentLine.right(currentLine.length()-3);
					currentPoly->setName(name);
				}
			}
			else if (currentLine.startsWith("CP"))
			{
				QStringList tokens = currentLine.split(QRegExp("\\s+"),QString::SkipEmptyParts);

				switch (cpIndex)
				{
				case 0: //first 'CP' line
					{
						//expected: CP + 'connected' + 'closed' flags
						bool ok = (tokens.size() == 3);
						if (ok)
						{
							bool ok1 = true, ok2 = true;
							int isConnected = tokens[1].toInt(&ok1);
							int isClosed = tokens[2].toInt(&ok2);
							ok = ok1 && ok2;
							if (ok)
							{
								if (isConnected == 0)
								{
									//points are not connected?!
									//--> we simply hide the polyline and display its vertices
									currentPoly->setVisible(false);
									currentVertices->setEnabled(true);
								}
								currentPoly->setClosed(isClosed != 0);
							}
						}
						if (!ok)
						{
							ccLog::Warning(QString("[SinusX] Line %1 is corrupted (expected: 'CP connected_flag closed_flag')").arg(lineNumber));
							result = CC_FERR_MALFORMED_FILE;
							continue;
						}
						++cpIndex;
					}
					break;

				case 1: //second 'CP' line
					{
						if (curveType == CUREV_S)
						{
							++cpIndex;
							//no break: we go directly to the next case (cpIndex = 2)
						}
						else if (curveType == CURVE_P)
						{
							//nothing particular for profiles (they are not handled in the same way in CC!)
							++cpIndex;
							break;
						}
						else if (curveType == CURVE_N)
						{
							//expected: CP + const_altitude
							bool ok = (tokens.size() == 2);
							if (ok)
							{
								double z = tokens[1].toDouble(&ok);
								if (ok)
									currentPoly->setMetaData(ccPolyline::MetaKeyConstAltitude(),QVariant(z));
							}
							if (!ok)
							{
								ccLog::Warning(QString("[SinusX] Line %1 is corrupted (expected: 'CP const_altitude')").arg(lineNumber));
								result = CC_FERR_MALFORMED_FILE;
								continue;
							}
							++cpIndex;
							break;
						}
						else if (curveType == CURVE_C)
						{
							//skip the next 16 values
							int skipped = tokens.size()-1; //all but the 'CP' keyword
							while (skipped < 16 && !currentLine.isEmpty() && file.error() == QFile::NoError)
							{
								currentLine = stream.readLine();
								++lineNumber;
								tokens = currentLine.split(QRegExp("\\s+"),QString::SkipEmptyParts);
								skipped += tokens.size();
							}
							assert(skipped == 16); //no more than 16 normally!
							++cpIndex;
							break;
						}
						else
						{
							assert(false);
							++cpIndex;
							break;
						}
					}

				case 2:
					{
						//CP + base plane: 0 = (XY), 1 = (YZ), 2 = (ZX)
						bool ok = (tokens.size() == 2);
						if (ok)
						{
							int vertDir = 2;
							QChar basePlaneChar = tokens[1].at(0);
							if (basePlaneChar == '0')
								vertDir = 2;
							else if (basePlaneChar == '1')
								vertDir = 0;
							else if (basePlaneChar == '2')
								vertDir = 1;
							else
								ok = false;
							if (ok)
								currentPoly->setMetaData(ccPolyline::MetaKeyUpDir(),QVariant(vertDir));
						}
						if (!ok)
						{
							ccLog::Warning(QString("[SinusX] Line %1 is corrupted (expected: 'CP base_plane')").arg(lineNumber));
							result = CC_FERR_MALFORMED_FILE;
							continue;
						}
					}
					++cpIndex;
					break;

				default:
					//ignored
					break;
				}
			}
			else if (!currentLine.isEmpty())
			{
				assert(currentVertices);

				//shoud be a point!
				QStringList tokens = currentLine.split(QRegExp("\\s+"),QString::SkipEmptyParts);
				bool ok = (tokens.size() == 4);
				if (ok)
				{
					CCVector3d Pd;
					Pd.x = tokens[0].toDouble(&ok);
					if (ok)
					{
						Pd.y = tokens[1].toDouble(&ok);
						if (ok)
						{
							Pd.z = tokens[2].toDouble(&ok);
							if (ok)
							{
								//resize vertex cloud if necessary
								if (	currentVertices->size() == currentVertices->capacity()
									&&	!currentVertices->reserve(currentVertices->size() + 10))
								{
									delete currentPoly;
									return CC_FERR_NOT_ENOUGH_MEMORY;
								}
								//first point: check for 'big' coordinates
								if (firstVertex/*currentVertices->size() == 0*/)
								{
									firstVertex = false;
									if (HandleGlobalShift(Pd,Pshift,parameters))
									{
										if (currentPoly)
											currentPoly->setGlobalShift(Pshift);
										else
											currentVertices->setGlobalShift(Pshift);
										ccLog::Warning("[SinusX::loadFile] Polyline has been recentered! Translation: (%.2f,%.2f,%.2f)",Pshift.x,Pshift.y,Pshift.z);
									}
								}

								currentVertices->addPoint(CCVector3::fromArray((Pd+Pshift).u));
							}
						}
					}
				}
				if (!ok)
				{
					ccLog::Warning(QString("[SinusX] Line %1 is corrupted (expected: 'X Y Z Key ...')").arg(lineNumber));
					result = CC_FERR_MALFORMED_FILE;
					continue;
				}
			}
		}
	}

	//don't forget the last polyline!
	if (currentPoly)
	{
		if (	currentVertices
			&&	currentVertices->size() != 0
			&&	currentVertices->resize(currentVertices->size())
			&&	currentPoly->addPointIndex(0,currentVertices->size()) )
		{
			container.addChild(currentPoly);
		}
	}

	return result;
}
