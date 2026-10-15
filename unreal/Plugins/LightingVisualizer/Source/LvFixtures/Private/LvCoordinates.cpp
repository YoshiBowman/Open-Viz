#include "LvCoordinates.h"

namespace LvCoordinates
{
	FTransform FromGdtf(const viz::Matrix4& Matrix)
	{
		// The file's matrix acts on column vectors; mirror it (S * M * S, S = diag(1, -1, 1)) and
		// transpose into Unreal's row-vector layout.
		static const double Mirror[3] = {1.0, -1.0, 1.0};
		FMatrix Unreal = FMatrix::Identity;
		for (int32 Row = 0; Row < 3; ++Row)
		{
			for (int32 Column = 0; Column < 3; ++Column)
			{
				Unreal.M[Column][Row] = Mirror[Row] * Matrix.m[Row][Column] * Mirror[Column];
			}
			Unreal.M[3][Row] = Mirror[Row] * Matrix.m[Row][3] * 100.0;
		}
		return FTransform(Unreal);
	}

	FTransform FromMvr(const viz::Transform& Transform)
	{
		// The object's own Y axis is mirrored as well as the world's, hence the double negation on V.
		FMatrix Unreal = FMatrix::Identity;
		const double* Rows[3] = {Transform.u, Transform.v, Transform.w};
		static const double RowSign[3] = {1.0, -1.0, 1.0};
		for (int32 Row = 0; Row < 3; ++Row)
		{
			Unreal.M[Row][0] = RowSign[Row] * Rows[Row][0];
			Unreal.M[Row][1] = RowSign[Row] * -Rows[Row][1];
			Unreal.M[Row][2] = RowSign[Row] * Rows[Row][2];
		}
		Unreal.M[3][0] = Transform.o[0] / 10.0;
		Unreal.M[3][1] = -Transform.o[1] / 10.0;
		Unreal.M[3][2] = Transform.o[2] / 10.0;
		return FTransform(Unreal);
	}

	viz::Transform ToMvr(const FTransform& Transform)
	{
		// FromMvr, undone term by term.
		const FMatrix Unreal = Transform.ToMatrixNoScale();
		viz::Transform Out;
		double* Rows[3] = {Out.u, Out.v, Out.w};
		static const double RowSign[3] = {1.0, -1.0, 1.0};
		for (int32 Row = 0; Row < 3; ++Row)
		{
			Rows[Row][0] = RowSign[Row] * Unreal.M[Row][0];
			Rows[Row][1] = RowSign[Row] * -Unreal.M[Row][1];
			Rows[Row][2] = RowSign[Row] * Unreal.M[Row][2];
		}
		Out.o[0] = Unreal.M[3][0] * 10.0;
		Out.o[1] = -Unreal.M[3][1] * 10.0;
		Out.o[2] = Unreal.M[3][2] * 10.0;
		return Out;
	}

	FQuat AxisRotation(char Axis, double Degrees)
	{
		const double Radians = FMath::DegreesToRadians(Degrees);
		const double C = FMath::Cos(Radians);
		const double S = FMath::Sin(Radians);
		viz::Matrix4 Matrix;
		if (Axis == 'X')
		{
			Matrix.m[1][1] = C;
			Matrix.m[1][2] = -S;
			Matrix.m[2][1] = S;
			Matrix.m[2][2] = C;
		}
		else
		{
			Matrix.m[0][0] = C;
			Matrix.m[0][1] = -S;
			Matrix.m[1][0] = S;
			Matrix.m[1][1] = C;
		}
		return FromGdtf(Matrix).GetRotation();
	}
}

std::string LvToStd(const FString& Text)
{
	return std::string(TCHAR_TO_UTF8(*Text));
}

FString LvToFString(const std::string& Text)
{
	return FString(UTF8_TO_TCHAR(Text.c_str()));
}
